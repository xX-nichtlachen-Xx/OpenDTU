// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022-2026 nichtlachen
 */

/*
Base class for firmware-update commands used to drive the inverter's
program-download protocol.
*/
#include "FirmwareCommand.h"
#include "../inverters/InverterAbstract.h"
#include "crc.h"

FirmwareCommand::FirmwareCommand(InverterAbstract* inv, const uint64_t router_address)
    : CommandAbstract(inv, router_address)
{
    _payload[0] = 0x0E;
    _payload[9] = 0x00;
    _payload_size = 10;

    setTimeout(2000);
}

const uint8_t* FirmwareCommand::getDataPayload()
{
    uint8_t checksum = 0;
    for (uint8_t i = 0; i < _payload_size; ++i) {
        checksum ^= _payload[i];
    }

    _payload[_payload_size] = checksum;
    return _payload;
}

uint8_t FirmwareCommand::getDataSize() const
{
    return _payload_size + 1;
}

void FirmwareCommand::udpateCRC(const uint8_t start_index, const uint8_t len)
{
    (void)start_index;
    (void)len;
}

bool FirmwareCommand::handleResponse(const fragment_t fragment[], const uint8_t max_fragment_id)
{
    const uint8_t expectedCmd = _payload[0] | 0x80;

    for (uint8_t i = 0; i < max_fragment_id; i++) {
        if (fragment[i].mainCmd != expectedCmd) {
            return false;
        }
    }

    return true;
}
