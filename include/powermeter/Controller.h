// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <powermeter/Provider.h>
#include <TaskSchedulerDeclarations.h>
#include <memory>
#include <mutex>

namespace PowerMeters {

class Controller {
public:
    void init(Scheduler& scheduler);

    void updateSettings();

    float getPowerTotal() const;
    // Returns per-phase power if the provider has that data, else nullopt.
    // phase: 1=L1, 2=L2, 3=L3
    std::optional<float> getPowerPhase(uint8_t phase) const;
    uint32_t getLastUpdate() const;
    bool isDataValid() const;

private:
    void loop();

    Task _loopTask;
    mutable std::mutex _mutex;
    std::unique_ptr<Provider> _upProvider = nullptr;
};

} // namespace PowerMeters

extern PowerMeters::Controller PowerMeter;
