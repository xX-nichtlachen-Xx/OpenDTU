// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2024-2026 Thomas Basler and others
 */
#include "GridProfileWriteCommand.h"
#include "../inverters/InverterAbstract.h"
#include "../parser/GridProfileParser.h"
#include "crc.h"
#include <algorithm>
#include <cstring>
#include <esp_log.h>

#undef TAG
static const char* TAG = "hoymiles";

// Chunk size mirrors ActiveDTU::writeGridProfile() (16 bytes per frame).
#define GRID_PROFILE_CHUNK_SIZE 16

// Timeouts. Middle frames get a very short timeout because no reply is
// expected; the last frame gets a generous window because the inverter
// commits the profile to flash before replying.
#define GRID_PROFILE_MID_TIMEOUT_MS 50
#define GRID_PROFILE_LAST_TIMEOUT_MS 2000

GridProfileWriteCommand::GridProfileWriteCommand(InverterAbstract* inv, const uint64_t router_address)
    : CommandAbstract(inv, router_address)
{
    _payload[0] = 0x0A;
    _payload[9] = 0x00;
    _payload_size = 10;
    setTimeout(GRID_PROFILE_MID_TIMEOUT_MS);
}

String GridProfileWriteCommand::getCommandName() const
{
    char buf[40];
    snprintf(buf, sizeof(buf), "GridProfileWrite (nub=%u%s)",
        static_cast<unsigned>(_packetNumber),
        _isLast ? ", LAST" : "");
    return buf;
}

void GridProfileWriteCommand::setPacketNumber(const uint8_t packetNumber, const bool isLast)
{
    _packetNumber = packetNumber;
    _isLast = isLast;
    _payload[9] = static_cast<uint8_t>(packetNumber | (isLast ? 0x80 : 0x00));
    setTimeout(isLast ? GRID_PROFILE_LAST_TIMEOUT_MS : GRID_PROFILE_MID_TIMEOUT_MS);
}

void GridProfileWriteCommand::setPayload(const uint8_t* data, const uint8_t len)
{
    const uint8_t safe = std::min<uint8_t>(len, GRID_PROFILE_CHUNK_SIZE);
    memcpy(&_payload[10], data, safe);
    _chunkLen = safe;
    _payload_size = 10 + safe;
    _crc16Applied = false;
    // Append the trailing CRC16 immediately on the last frame so it is baked
    // into _payload / _payload_size before getDataPayload() runs.
    appendCrc16IfLast();
}

void GridProfileWriteCommand::setFullProfile(const uint8_t* profile, const size_t profileLen)
{
    _fullProfile.assign(profile, profile + profileLen);
}

void GridProfileWriteCommand::appendCrc16IfLast()
{
    if (!_isLast || _crc16Applied || _fullProfile.empty()) {
        return;
    }
    // Room check: header(10) + chunk + 2 (CRC16) + 1 (crc8) must fit in RF_LEN.
    if (10 + _chunkLen + 2 + 1 > RF_LEN) {
        ESP_LOGE(TAG, "GridProfileWrite: last chunk too large for inline CRC16");
        return;
    }
    const uint16_t c = crc16(_fullProfile.data(), _fullProfile.size());
    _payload[10 + _chunkLen] = static_cast<uint8_t>(c >> 8);
    _payload[10 + _chunkLen + 1] = static_cast<uint8_t>(c & 0xff);
    _payload_size = 10 + _chunkLen + 2;
    _crc16Applied = true;
}

bool GridProfileWriteCommand::handleResponse(const fragment_t fragment[], const uint8_t max_fragment_id)
{
    // Middle frames: no reply expected, so any packet arriving is unexpected.
    if (!_isLast) {
        return false;
    }

    // Dump every incoming fragment so we can see the raw response even when
    // the inverter answers with an unexpected mainCmd (e.g. 0x02 alarm frame).
    for (uint8_t i = 0; i < max_fragment_id; i++) {
        char hex[3 * MAX_RF_PAYLOAD_SIZE + 1];
        size_t off = 0;
        for (uint8_t j = 0; j < fragment[i].len && off + 3 < sizeof(hex); j++) {
            off += snprintf(&hex[off], sizeof(hex) - off, "%02X ", fragment[i].fragment[j]);
        }
        hex[off] = '\0';
        ESP_LOGI(TAG, "GridProfileWrite RX frag[%u] mainCmd=0x%02X len=%u: %s",
            i, fragment[i].mainCmd, fragment[i].len, hex);
    }

    for (uint8_t i = 0; i < max_fragment_id; i++) {
        if (fragment[i].mainCmd != (_payload[0] | 0x80)) {
            ESP_LOGW(TAG, "GridProfileWrite: unexpected mainCmd 0x%02X (want 0x%02X)",
                fragment[i].mainCmd, static_cast<uint8_t>(_payload[0] | 0x80));
            _inv->GridProfile()->setLastWriteCommandSuccess(CMD_NOK);
            return false;
        }
    }

    // The gateway reference (usart_nrf.c handleResponse for 0x8A) reads state
    // at raw offset 10 (nub byte). After the 10-byte header strip, that maps
    // to fragment.fragment[0]. Some devices instead put status one byte later.
    uint8_t state = 0xFF;
    if (max_fragment_id > 0 && fragment[0].len >= 1) {
        state = fragment[0].fragment[0];
    }

    if (state == 0x00) {
        ESP_LOGI(TAG, "GridProfileWrite: inverter reported success (state=0)");
        _inv->GridProfile()->setLastWriteCommandSuccess(CMD_OK);
        _inv->onGridProfileWriteCompleted(true);
        return true;
    }

    ESP_LOGW(TAG, "GridProfileWrite: inverter reported failure (state=0x%02X)", state);
    _inv->GridProfile()->setLastWriteCommandSuccess(CMD_NOK);
    _inv->onGridProfileWriteCompleted(false);
    // Return true so the command is considered "handled" and the queue advances
    // instead of trying to retransmit; the failure has already been recorded.
    return true;
}

void GridProfileWriteCommand::gotTimeout()
{
    if (_isLast) {
        ESP_LOGW(TAG, "GridProfileWrite: timeout waiting for ack on last frame");
        _inv->GridProfile()->setLastWriteCommandSuccess(CMD_NOK);
        _inv->onGridProfileWriteCompleted(false);
    }
    CommandAbstract::gotTimeout();
}

uint8_t GridProfileWriteCommand::getMaxResendCount() const
{
    // Middle frames go once; only the last frame is worth resending because
    // it's the one the inverter actually acks. Resending a middle frame while
    // the queue still holds later frames would break ordering.
    return _isLast ? 3 : 0;
}

uint8_t GridProfileWriteCommand::getMaxRetransmitCount() const
{
    // We never request per-fragment retransmit for write frames.
    return 0;
}
