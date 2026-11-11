// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 OpenDTU contributors
 */
#include "SeedCommand.h"
#include "../inverters/InverterAbstract.h"
#include <cstring>

SeedCommand::SeedCommand(InverterAbstract* inv, const uint64_t router_address)
    : CommandAbstract(inv, router_address)
{
    _payload[0] = 0x35;
    _payload[9] = 0x81;

    inv->generateEncryptionSeed();
    memcpy(&_payload[10], inv->getEncryptionSeed(), 16);

    _payload_size = 26;

    setTimeout(500);
}

String SeedCommand::getCommandName() const
{
    return "SeedExchange";
}

bool SeedCommand::handleResponse(const fragment_t fragment[], const uint8_t max_fragment_id)
{
    _inv->setEncryptionSessionActive(true);
    return true;
}

void SeedCommand::gotTimeout()
{
}
