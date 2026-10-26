#pragma once

#include "MultiDataCommand.h"

class SelfCheckStateCommand : public MultiDataCommand {
public:
    explicit SelfCheckStateCommand(InverterAbstract* inv, const uint64_t router_address = 0, const time_t time = 0);

    virtual String getCommandName() const;

    virtual bool handleResponse(const fragment_t fragment[], const uint8_t max_fragment_id);
    virtual void handlePartialResponse(const fragment_t fragment[], const uint8_t last_fragment_id, const uint8_t max_fragment_id);

private:
    void logResult();
};
