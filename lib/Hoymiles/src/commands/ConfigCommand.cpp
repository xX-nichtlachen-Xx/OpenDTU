// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 OpenDTU contributors
 */
#include "ConfigCommand.h"
#include "../inverters/InverterAbstract.h"
#include "crc.h"

ConfigCommand::ConfigCommand(InverterAbstract* inv, const uint64_t router_address)
    : CommandAbstract(inv, router_address)
{
    _payload[0] = 0x36;
    _payload[9] = 0x81;

    _payload[14] = 0x01;
    for (uint8_t i = 15; i <= 23; i++) {
        _payload[i] = 0x00;
    }
    setTime(0);

    _payload_size = 26;

    setTimeout(500);
}

void ConfigCommand::setTime(const time_t time)
{
    _payload[10] = static_cast<uint8_t>(time >> 24);
    _payload[11] = static_cast<uint8_t>(time >> 16);
    _payload[12] = static_cast<uint8_t>(time >> 8);
    _payload[13] = static_cast<uint8_t>(time);
    updateCRC();
}

void ConfigCommand::updateCRC()
{
    const uint16_t crc = crc16(&_payload[10], 14);
    _payload[24] = static_cast<uint8_t>(crc >> 8);
    _payload[25] = static_cast<uint8_t>(crc);
}

String ConfigCommand::getCommandName() const
{
    return "ConfigExchange";
}

bool ConfigCommand::handleResponse(const fragment_t fragment[], const uint8_t max_fragment_id)
{
    _inv->setEncryptionSessionActive(true);
    return true;
}

void ConfigCommand::gotTimeout()
{
}
