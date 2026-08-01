// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../commands/CommandAbstract.h"
#include <ThreadSafeQueue.h>
#include <memory>

class InverterAbstract;

class CommandQueue : public ThreadSafeQueue<std::shared_ptr<CommandAbstract>> {
public:
    void removeAllEntriesForInverter(InverterAbstract* inv);
    void removeDuplicatedEntries(std::shared_ptr<CommandAbstract> cmd);
    void replaceEntries(std::shared_ptr<CommandAbstract> cmd);

    uint8_t countSimilarCommands(std::shared_ptr<CommandAbstract> cmd);

    // Removes every pending (i.e. not-currently-executing) GridProfileWrite
    // command targeting `inv` from the queue. The currently executing entry
    // at the front of the queue is NEVER touched to avoid corrupting the
    // radio state machine mid-transfer.
    uint8_t removePendingGridProfileWriteCommands(InverterAbstract* inv);

    // Returns true if any GridProfileWrite command targeting `inv` is present
    // in the queue (front or pending).
    bool hasGridProfileWriteCommands(InverterAbstract* inv);
};
