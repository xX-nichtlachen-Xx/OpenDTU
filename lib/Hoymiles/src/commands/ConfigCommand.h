// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "CommandAbstract.h"
#include <sys/time.h>

/*
ConfigCommand
*/
class ConfigCommand : public CommandAbstract {
public:
    explicit ConfigCommand(InverterAbstract* inv, const uint64_t router_address = 0);

    void setTime(const time_t time);

    virtual String getCommandName() const;
    virtual bool handleResponse(const fragment_t fragment[], const uint8_t max_fragment_id);
    virtual void gotTimeout();

    // The 0x36 block is encrypted with the seed-only config key channel.
    bool isEncryptable() const override { return true; }
    bool usesConfigChannel() const override { return true; }

private:
    void updateCRC();
};
