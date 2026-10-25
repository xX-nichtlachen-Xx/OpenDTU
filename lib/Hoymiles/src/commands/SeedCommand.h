// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "CommandAbstract.h"

/*
SeedCommand — the 0x35 encryption handshake (HM 1.5.x and later).
*/
class SeedCommand : public CommandAbstract {
public:
    explicit SeedCommand(InverterAbstract* inv, const uint64_t router_address = 0);

    virtual String getCommandName() const;
    virtual bool handleResponse(const fragment_t fragment[], const uint8_t max_fragment_id);
    virtual void gotTimeout();

    bool isEncryptable() const override { return false; }
};
