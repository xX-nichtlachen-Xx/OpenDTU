// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "FirmwareCommand.h"
#include <vector>

class FirmwareDataCommand : public FirmwareCommand {
public:
    explicit FirmwareDataCommand(InverterAbstract* inv, const uint64_t router_address = 0);

    String getCommandName() const override;
    void setPacketNumber(const uint8_t packet_no);
    void setPayload(const uint8_t* data, const uint8_t len);
    void setRowData(const uint8_t* rowData, const uint8_t rowLen);

    // Appends the 2-byte big-endian CRC16 (Modbus, start 0xFFFF) computed over
    // the FULL Intel-Hex row (rowData/rowLen), not just this packet's chunk.
    // Only called for the LAST packet of a row -- matches the reference
    // gateway's UsartNrf_Send_MiProgram_SigleFrame(), which only appends this
    // CRC16 when the nub byte has the 0x80 (last-of-row) bit set.
    void appendRowCrc(const uint8_t* rowData, const uint8_t rowLen);

    // Firmware update has thousands of packets; keep a stalled transfer from
    // wasting time waiting out the default 4 resend attempts per packet.
    uint8_t getMaxResendCount() const override;

    bool isFirmwareDataCommand() const override { return true; }

    bool handleResponse(const fragment_t fragment[], const uint8_t max_fragment_id) override;
    void gotTimeout() override;

    // Records which attempt (1 = first try) this instance's row is on, so
    // gotTimeout() knows whether to resend the row again or give up.
    void setRowAttempt(const uint8_t attempt);

private:
    // Copy of the row this packet completes (only set for the last packet of
    // a row); used to verify the ack and to resend the whole row on failure.
    std::vector<uint8_t> _rowData;
    uint8_t _rowAttempt = 1;
};
