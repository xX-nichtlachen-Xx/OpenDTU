// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "CommandAbstract.h"
#include <cstdint>

/*
Command used to write the per-string YieldTotal (total produced energy, Wh)
to a Hoymiles inverter via the 0x52 "ParaSet" mainCmd.

  Frame 1 (nub=0x01, not last):
    [9]      0x01
    [10..13] 01 00 00 0F   (fixed subcmd/header)
    [14..17] string1 Wh (u32 BE)
    [18..21] string2 Wh (u32 BE, 0 if unused)
    [22..25] string3 Wh (u32 BE, 0 if unused)
  Frame 2 (nub=0x82, last):
    [9]      0x82
    [10..13] string4 Wh (u32 BE, 0 if unused)
    [14..15] CRC16 over frame1[10..25] (16 bytes) + frame2[10..13] (4 bytes)

The inverter answers only on the last frame with mainCmd 0xD2. The response
payload (after the 10-byte header strip) carries a state byte at offset 0
(0 == accepted) and a 16-bit "sub echo" at offset 2..3 which must read 0x0001
for a successful write.
*/

class YieldTotalSetCommand : public CommandAbstract {
public:
    explicit YieldTotalSetCommand(InverterAbstract* inv, const uint64_t router_address = 0);

    virtual String getCommandName() const;

    // valuesWh holds up to 4 per-string Wh totals (index 0..3). Unused string
    // slots must be 0. frameIndex is 1 for the first frame, 2 for the last.
    void setValues(const uint32_t valuesWh[4], const uint8_t frameIndex);

    virtual bool handleResponse(const fragment_t fragment[], const uint8_t max_fragment_id);
    virtual void gotTimeout();

    virtual uint8_t getMaxResendCount() const;
    virtual uint8_t getMaxRetransmitCount() const;
    virtual QueueInsertType getQueueInsertType() const { return QueueInsertType::AllowMultiple; }

private:
    bool _isLast = false;
};
