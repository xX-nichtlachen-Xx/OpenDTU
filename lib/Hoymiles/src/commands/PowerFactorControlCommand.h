// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "ActivePowerControlCommand.h"
#include "DevControlCommand.h"

class PowerFactorControlCommand : public DevControlCommand {
public:
    explicit PowerFactorControlCommand(InverterAbstract* inv, const uint64_t router_address = 0);

    virtual String getCommandName() const;
    virtual QueueInsertType getQueueInsertType() const { return QueueInsertType::RemoveOldest; }
    virtual bool areSameParameter(CommandAbstract* other);

    virtual bool handleResponse(const fragment_t fragment[], const uint8_t max_fragment_id);
    virtual void gotTimeout();

    // Reuses the same 4-state persistent/relative enum used by active/reactive power.
    void setPowerFactorLimit(const float pf, const PowerLimitControlType type = RelativPersistent);
    float getLimit() const;
    PowerLimitControlType getType() const;

private:
    static uint16_t getControlTypeValue(PowerLimitControlType type);
};
