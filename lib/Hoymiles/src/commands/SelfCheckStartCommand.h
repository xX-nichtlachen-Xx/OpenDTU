#pragma once

#include "DevControlCommand.h"

class SelfCheckStartCommand : public DevControlCommand {
public:
    explicit SelfCheckStartCommand(InverterAbstract* inv, const uint64_t router_address = 0);

    virtual String getCommandName() const;

    virtual bool handleResponse(const fragment_t fragment[], const uint8_t max_fragment_id);
    virtual void gotTimeout();
};
