// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "Configuration.h"
#include "PowerLimiterInverter.h"
#include <espMqttClient.h>
#include <Arduino.h>
#include <atomic>
#include <deque>
#include <memory>
#include <functional>
#include <optional>
#include <TaskSchedulerDeclarations.h>
#include <frozen/string.h>

#define PL_UI_STATE_INACTIVE 0
#define PL_UI_STATE_CHARGING 1
#define PL_UI_STATE_USE_SOLAR_ONLY 2
#define PL_UI_STATE_USE_SOLAR_AND_BATTERY 3

class PowerLimiterClass {
public:
    PowerLimiterClass() = default;

    enum class Status : unsigned {
        Initializing,
        DisabledByConfig,
        DisabledByMqtt,
        WaitingForValidTimestamp,
        PowerMeterPending,
        InverterInvalid,
        InverterCmdPending,
        ConfigReload,
        InverterStatsPending,
        UnconditionalSolarPassthrough,
        Stable,
    };

    void init(Scheduler& scheduler);
    void triggerReloadingConfig() { _reloadConfigFlag = true; }
    uint8_t getInverterUpdateTimeouts() const;
    uint8_t getPowerLimiterState() const;
    int32_t getInverterOutput() const { return _lastExpectedInverterOutput; }
    bool isFullSolarPassthroughActive() const { return _fullSolarPassThroughActive; }

    enum class Mode : unsigned {
        Normal = 0,
        Disabled = 1,
        UnconditionalFullSolarPassthrough = 2
    };

    void setMode(Mode m) { _mode = m; _reloadConfigFlag = true; }
    Mode getMode() const { return _mode; }
    bool usesBatteryPoweredInverter() const;
    bool usesSmartBufferPoweredInverter() const;

    // used to interlock Huawei R48xx grid charger against battery-powered inverters
    bool isGovernedBatteryPoweredInverterProducing() const;

private:
    void loop();

    Task _loopTask;

    std::atomic<bool> _reloadConfigFlag = true;
    uint16_t _lastExpectedInverterOutput = 0;
    Status _lastStatus = Status::Initializing;
    uint32_t _lastStatusPrinted = 0;
    uint32_t _lastCalculation = 0;
    static constexpr uint32_t _calculationBackoffMsDefault = 128;
    uint32_t _calculationBackoffMs = _calculationBackoffMsDefault;
    Mode _mode = Mode::Normal;

    std::deque<std::unique_ptr<PowerLimiterInverter>> _inverters;
    std::deque<std::unique_ptr<PowerLimiterInverter>> _retirees;

    enum class BatteryState : uint8_t { STOP = 0, NO_DISCHARGE = 1, DISCHARGE_ALLOWED = 2, DISCHARGE_NIGHT = 3 };
    BatteryState _batteryState = BatteryState::STOP;
    bool _fromStart = false;
    bool _oneStopPerNightDone = false;

    std::pair<bool, uint32_t> _nextInverterRestart = { false, 0 };
    bool _fullSolarPassThroughActive = false;
    float _loadCorrectedVoltage = 0.0f;

    frozen::string const& getStatusText(Status status) const;
    void announceStatus(Status status);
    void reloadConfig();
    std::pair<float, char const*> getInverterDcVoltage() const;
    float getBatteryVoltage(bool log = false) const;
    uint16_t dcPowerBusToInverterAc(uint16_t dcPower) const;
    void unconditionalFullSolarPassthrough();
    // phase: 0=Total (sum), 1=L1, 2=L2, 3=L3
    uint16_t calcTargetOutput(uint8_t phase) const;
    using inverter_filter_t = std::function<bool(PowerLimiterInverter const&)>;
    uint16_t updateInverterLimits(uint16_t powerRequested, inverter_filter_t filter, std::string const& filterExpression);
    uint16_t calcPowerBusUsage(uint16_t powerRequested) const;
    bool updateInverters();
    uint16_t getSolarPassthroughPower() const;
    std::optional<uint16_t> getBatteryDischargeLimit() const;
    float getBatteryInvertersOutputAcWatts() const;

    // Returns meter value for the given phase (1=L1,2=L2,3=L3) or
    // falls back to PowerTotal when phase data is unavailable.
    float getMeterValueForPhase(uint8_t phase) const;

    // One regulation pass for a single phase (or Total=0).
    // Returns covered watts and fills residual via reference.
    // dcBusBudgetRemainingAc is the shared DC bus budget (solar passthrough
    // + battery allowance) in AC watts.  It is decremented as battery-
    // powered inverters consume from it across successive phase passes.
    // globalAllowanceRemainingAc is the shared TotalUpperPowerLimit budget
    // in AC watts.  It is decremented as any inverter type covers demand,
    // enforcing the global cap across all phases.
    uint16_t regulatePhase(uint8_t phase, int16_t& residual,
                           uint16_t& dcBusBudgetRemainingAc,
                           uint16_t& globalAllowanceRemainingAc,
                           uint16_t maxNegOvershoot);

    bool testThreshold(float socThreshold, float voltThreshold,
            std::function<bool(float, float)> compare) const;
    bool isStartThresholdReached() const;
    bool isStopThresholdReached() const;
    bool isBelowStopThreshold() const;
    void calcNextInverterRestart();
    bool isSolarPassThroughEnabled() const;
};

extern PowerLimiterClass PowerLimiter;
