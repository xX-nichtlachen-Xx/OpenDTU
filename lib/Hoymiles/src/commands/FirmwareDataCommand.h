// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "FirmwareCommand.h"
#include <vector>

class FirmwareDataCommand : public FirmwareCommand {
public:
    explicit FirmwareDataCommand(InverterAbstract* inv, const uint64_t router_address = 0);

    String getCommandName() const override;
    void setPacketNumber(const uint8_t packet_no);

    static uint8_t rowAckResendCount(const uint8_t recordType);

    void setRowAckResendCount(const uint8_t count);
    void setPayload(const uint8_t* data, const uint8_t len);
    void setRowData(const uint8_t* rowData, const uint8_t rowLen);

    void appendRowCrc(const uint8_t* rowData, const uint8_t rowLen);

    uint8_t getMaxResendCount() const override;

    bool expectsResponse() const override;
    bool isFirmwareDataCommand() const override { return true; }

    bool handleResponse(const fragment_t fragment[], const uint8_t max_fragment_id) override;
    void gotTimeout() override;

    void setRowAttempt(const uint8_t attempt);

private:
    std::vector<uint8_t> _rowData;
    uint8_t _rowAttempt = 1;
    uint8_t _rowAckResendCount = 0; // 0 = default MAX_ATTEMPTS_PER_LINE
};
