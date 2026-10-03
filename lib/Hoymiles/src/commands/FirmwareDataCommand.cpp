// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022-2026 nichtlachen
 */

/*
Sends a firmware data fragment using the inverter's program-download protocol.
*/
#include "FirmwareDataCommand.h"
#include "../crc.h"
#include "../inverters/InverterAbstract.h"
#include "../Utils.h"
#include <algorithm>
#include <cstring>
#include <esp_log.h>

#undef TAG
static const char* TAG = "hoymiles";

#define MAX_PAYLOAD_SIZE 16
#define MAX_ATTEMPTS_PER_LINE 10

#define ROW_ACK_TIMEOUT_MS 350 // per end-packet wait, all row types

#define ROW_ACK_RESENDS_DATA MAX_ATTEMPTS_PER_LINE // ~3.5 s
#define ROW_ACK_RESENDS_ERASE 23 // ~8 s: address / leading vendor rows -> flash erase
#define ROW_ACK_RESENDS_EOF 17 // ~6 s: EOF row -> commit / verify

FirmwareDataCommand::FirmwareDataCommand(InverterAbstract* inv, const uint64_t router_address)
    : FirmwareCommand(inv, router_address)
{
    _payload[0] = 0x0E; // DOWN_PRO -- the only command id used for the whole download
    _payload[9] = 0x00; // nub (packet number), set via setPacketNumber()

    _payload_size = 10;
    setTimeout(350);
}

String FirmwareDataCommand::getCommandName() const
{
    return "FirmwareData";
}

void FirmwareDataCommand::setPacketNumber(const uint8_t packet_no)
{
    _payload[9] = packet_no;

    setTimeout((packet_no & 0x80) ? ROW_ACK_TIMEOUT_MS : 30);
}

uint8_t FirmwareDataCommand::rowAckResendCount(const uint8_t recordType)
{
    switch (recordType) {
    case 0x00:
        return ROW_ACK_RESENDS_DATA;
    case 0x01:
        return ROW_ACK_RESENDS_EOF;
    default:
        return ROW_ACK_RESENDS_ERASE;
    }
}

void FirmwareDataCommand::setRowAckResendCount(const uint8_t count)
{
    if (_payload[9] & 0x80) {
        _rowAckResendCount = count;
    }
}

void FirmwareDataCommand::setPayload(const uint8_t* data, const uint8_t len)
{
    const uint8_t dataLen = static_cast<uint8_t>(std::min<size_t>(MAX_PAYLOAD_SIZE, len));

    memset(&_payload[10], 0, MAX_PAYLOAD_SIZE);
    memcpy(&_payload[10], data, dataLen);
    _payload_size = static_cast<uint8_t>(10 + dataLen);
}

void FirmwareDataCommand::setRowData(const uint8_t* rowData, const uint8_t rowLen)
{
    _rowData.assign(rowData, rowData + rowLen);
}

void FirmwareDataCommand::appendRowCrc(const uint8_t* rowData, const uint8_t rowLen)
{
    const uint16_t crc = crc16(rowData, rowLen, 0xFFFF);
    _payload[_payload_size++] = static_cast<uint8_t>(crc >> 8);
    _payload[_payload_size++] = static_cast<uint8_t>(crc & 0xFF);

    _rowData.assign(rowData, rowData + rowLen);
}

uint8_t FirmwareDataCommand::getMaxResendCount() const
{
    if ((_payload[9] & 0x80) == 0) {
        return 0;
    }
    return _rowAckResendCount != 0 ? _rowAckResendCount : MAX_ATTEMPTS_PER_LINE;
}

bool FirmwareDataCommand::expectsResponse() const
{
    return (_payload[9] & 0x80) != 0;
}

bool FirmwareDataCommand::handleResponse(const fragment_t fragment[], const uint8_t max_fragment_id)
{   
    if (!FirmwareCommand::handleResponse(fragment, max_fragment_id)) {
        return false;
    }
    //ESP_LOGI(TAG, "FirmwareDataCommand::handleResponse(): _rowData.empty = %d, max_fragment_id = %d", _rowData.empty(), max_fragment_id);
    if (_rowData.empty()) {
        ESP_LOGV(TAG, "FirmwareDataCommand::handleResponse(): not the last-of-row packet -- no per-row ack to verify");
        return true; // not the last-of-row packet -- no per-row ack to verify
    }
    if (max_fragment_id == 0) {
        return false; // last-of-row packet without any fragment -- let the retry path run
    }
    const fragment_t& ack = fragment[max_fragment_id - 1];
    const uint8_t headerLen = static_cast<uint8_t>(std::min<size_t>(4, _rowData.size()));
    const bool rowAckOk = ack.len >= headerLen && memcmp(ack.fragment, _rowData.data(), headerLen) == 0;
    if (rowAckOk) {
        _inv->onFirmwareRowCompleted();
    }
    return rowAckOk;
}

void FirmwareDataCommand::gotTimeout()
{
    if ((_payload[9] & 0x80) == 0) {
        return;
    }

    if (_rowData.empty() || _rowData.size() < 4) {
        ESP_LOGW(TAG, "FirmwareDataCommand::gotTimeout(): no resend context, aborting firmware update");
        _inv->failFirmwareUpdateRequest();
        return;
    }

    if (_rowAttempt < MAX_ATTEMPTS_PER_LINE) {
        ESP_LOGW(TAG, "FirmwareDataCommand::gotTimeout(): resending row attempt %d", _rowAttempt + 1);
        _inv->resendFirmwareRow(_rowData.data(), static_cast<uint16_t>(_rowData.size()), _rowAttempt + 1);
    } else {
        ESP_LOGW(TAG, "FirmwareDataCommand::gotTimeout(): max attempts reached, aborting firmware update");
        _inv->failFirmwareUpdateRequest();
    }
}

void FirmwareDataCommand::setRowAttempt(const uint8_t attempt)
{
    _rowAttempt = attempt;
}

