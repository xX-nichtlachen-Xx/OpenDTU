// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022-2026 nichtlachen
 */

/*
Sends a firmware data fragment using the inverter's program-download protocol.

Confirmed against the reference gateway firmware (usart_nrf.c:
UsartNrf_Send_PackNrfCmd() / UsartNrf_Send_MiProgram_SigleFrame()) and a real
hardware capture: there is NO separate "start"/"version"/"commit" command for
a firmware download. The ENTIRE transfer -- from the very first packet to the
very last -- uses the single command id DOWN_PRO (0x0E), answered with
ANSWER_DOWN_PRO (0x8E). E.g. captured TX frames:
  0E FF FF FF FF FF FF FF FF 81 06 00 00 11 10 11 00 00 27
  0E FF FF FF FF FF FF FF FF 81 02 00 00 10 00 FE BC 81 B7
The packet-number byte at offset 9 ("nub") starts at 1 and increments per
packet; only the LAST packet has bit 0x80 OR'd into it.
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

    // Intermediate chunks are fire-and-forget (no ack expected) -- keep the
    // per-packet wait tiny so we just let the RF layer finish, then move on.
    // Only the last packet of a row (0x80 bit set) waits for the row ack.
    setTimeout((packet_no & 0x80) ? 350 : 30);
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
    // Only the final packet of a row (nub bit 0x80) gets acked by the
    // inverter -- see UsartNrf_Send_MiProgram_SigleFrame() in the reference
    // gateway firmware. Intermediate chunks are fire-and-forget: 0 resends
    // avoids wasting time re-transmitting a chunk that will never be acked.
    if ((_payload[9] & 0x80) == 0) {
        return 0;
    }
    return MAX_ATTEMPTS_PER_LINE;
}

bool FirmwareDataCommand::expectsResponse() const
{
    // Intermediate chunks are fire-and-forget and do not require a response
    // fragment check. Only the final packet of a row expects a row-ack.
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
    // A successful row ack echoes this row's own header bytes (len, addr
    // hi/lo, record type); a failed CRC check on the inverter side instead
    // comes back as all-zero data -- gotTimeout() below resends the row.
    const fragment_t& ack = fragment[max_fragment_id - 1];
    const uint8_t headerLen = static_cast<uint8_t>(std::min<size_t>(4, _rowData.size()));
    const bool rowAckOk = ack.len >= headerLen && memcmp(ack.fragment, _rowData.data(), headerLen) == 0;
    if (rowAckOk) {
        // Row done -- inverter enqueues the next parked row.
        _inv->onFirmwareRowCompleted();
    }
    return rowAckOk;
}

void FirmwareDataCommand::gotTimeout()
{
    // Intermediate chunks (nub without 0x80) are fire-and-forget -- no ack
    if ((_payload[9] & 0x80) == 0) {
        return;
    }

    // Last-of-row packet but no per-row resend context (missing CRC info or
    // non-data record type) -- can't safely retry, abort the whole update
    // instead of letting the rest of the pre-enqueued rows keep firing.
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

