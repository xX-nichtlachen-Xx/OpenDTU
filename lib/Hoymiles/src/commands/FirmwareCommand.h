// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "CommandAbstract.h"

class FirmwareCommand : public CommandAbstract {
public:
    explicit FirmwareCommand(InverterAbstract* inv, const uint64_t router_address = 0);

    const uint8_t* getDataPayload() override;
    uint8_t getDataSize() const override;
    virtual bool handleResponse(const fragment_t fragment[], const uint8_t max_fragment_id);
    virtual QueueInsertType getQueueInsertType() const { return QueueInsertType::AllowMultiple; }

protected:
    void udpateCRC(const uint8_t start_index, const uint8_t len);
};
