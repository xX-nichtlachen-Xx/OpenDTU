// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "CommandAbstract.h"
#include <cstddef>
#include <vector>

/*
Command used to write a grid profile blob to a Hoymiles inverter.

Derives from CommandAbstract. Frame layout mirrors the DOWN_DAT / 0x0A gateway
frame in NRF/usart_nrf.c and the working ActiveDTU::writeGridProfile()
implementation:

  00   01 02 03 04   05 06 07 08   09     10..(10+N-1)   [+CRC16 BE]   crc8
  cmd  target x4     router x4     nub    payload         (last only)  trailer

* cmd is fixed 0x0A
* nub is the 1-based fragment index (1..N); the last fragment has 0x80 OR-ed on
* On the LAST fragment we append a big-endian Modbus CRC16 covering the WHOLE
  profile payload, then the standard crc8 trailer that CommandAbstract adds
  in getDataPayload() (this matches ActiveDTU's known-good behaviour on
  the RF-layer; XOR checksum in usart_nrf.c is a STM32<->nRF UART link
  concern, NOT the RF trailer)
* Chunk size is 16 bytes. If the last data chunk is already a full 16
  bytes, the trailing CRC16 cannot be appended inline (that would make an
  18-byte row) -- it is instead sent as its own extra, data-less, isLast
  frame, same split-CRC quirk firmware writes need.

The inverter answers only on the LAST frame with mainCmd == 0x8A and a state
byte inside the fragment payload. State byte 0 means the profile was accepted
and written to flash.

Middle frames are fire-and-forget: getMaxResendCount()==0 gives them a single
send and short timeout; the queue then advances to the next frame.
The last frame gets a longer timeout and a few resends because the inverter
performs an internal flash write before responding.
*/

class GridProfileWriteCommand : public CommandAbstract {
public:
    explicit GridProfileWriteCommand(InverterAbstract* inv, const uint64_t router_address = 0);

    virtual String getCommandName() const;

    // Sets the 1-based fragment index. If isLast is true, 0x80 is OR-ed on
    // and the last-fragment CRC16 handling is enabled in getDataPayload().
    void setPacketNumber(const uint8_t packetNumber, const bool isLast);

    // Copies chunk bytes into the payload area. Length must be <= 16.
    void setPayload(const uint8_t* data, const uint8_t len);

    // Stores the FULL profile buffer for computing the trailing CRC16 on the
    // last fragment. Must be called before getDataPayload() on the last frame.
    void setFullProfile(const uint8_t* profile, const size_t profileLen);

    virtual bool handleResponse(const fragment_t fragment[], const uint8_t max_fragment_id);
    virtual void gotTimeout();

    virtual uint8_t getMaxResendCount() const;
    virtual uint8_t getMaxRetransmitCount() const;
    virtual QueueInsertType getQueueInsertType() const { return QueueInsertType::AllowMultiple; }

    bool isGridProfileWriteCommand() const override { return true; }

    bool isLastFragment() const { return _isLast; }
    uint8_t getPacketNumber() const { return _packetNumber; }

private:
    void appendCrc16IfLast();

    uint8_t _packetNumber = 0;
    bool _isLast = false;
    uint8_t _chunkLen = 0;

    // Copy of the whole profile so the last frame can compute the CRC16 in
    // getDataPayload() regardless of when handleReceivedPackage() re-reads
    // the payload for a resend.
    std::vector<uint8_t> _fullProfile;
    bool _crc16Applied = false;
};
