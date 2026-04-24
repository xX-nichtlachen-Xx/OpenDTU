// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022 Thomas Basler and others
 */

#include <battery/Controller.h>
#include <battery/Stats.h>
#include <powermeter/Controller.h>
#include "PowerLimiter.h"
#include "Configuration.h"
#include "MqttSettings.h"
#include "NetworkSettings.h"
#include <gridcharger/Controller.h>
#include <solarcharger/Controller.h>
#include <ctime>
#include <cmath>
#include <limits>
#include <frozen/map.h>
#include "SunPosition.h"
#include <LogHelper.h>

#undef TAG
static const char* TAG = "dynamicPowerLimiter";
static const char* SUBTAG = "Controller";

static auto sBatteryPoweredFilter = [](PowerLimiterInverter const& inv) {
    return inv.isBatteryPowered();
};

static const char sBatteryPoweredExpression[] = "battery-powered";

static auto sSolarPoweredFilter = [](PowerLimiterInverter const& inv) {
    return inv.isSolarPowered();
};

static const char sSolarPoweredExpression[] = "solar-powered";

static auto sSmartBufferPoweredFilter = [](PowerLimiterInverter const& inv) {
    return inv.isSmartBufferPowered();
};

static const char sSmartBufferPoweredExpression[] = "smart-buffer-powered";

PowerLimiterClass PowerLimiter;

void PowerLimiterClass::init(Scheduler& scheduler)
{
    scheduler.addTask(_loopTask);
    _loopTask.setCallback(std::bind(&PowerLimiterClass::loop, this));
    _loopTask.setIterations(TASK_FOREVER);
    _loopTask.enable();
}

frozen::string const& PowerLimiterClass::getStatusText(PowerLimiterClass::Status status) const
{
    static const frozen::string missing = "programmer error: missing status text";

    static const frozen::map<Status, frozen::string, 11> texts = {
        { Status::Initializing, "initializing (should not see me)" },
        { Status::DisabledByConfig, "disabled by configuration" },
        { Status::DisabledByMqtt, "disabled by MQTT" },
        { Status::WaitingForValidTimestamp, "waiting for valid date and time to be available" },
        { Status::PowerMeterPending, "waiting for sufficiently recent power meter reading" },
        { Status::InverterInvalid, "invalid inverter selection/configuration" },
        { Status::InverterCmdPending, "waiting for a start/stop/restart/limit command to complete" },
        { Status::ConfigReload, "reloading DPL configuration" },
        { Status::InverterStatsPending, "waiting for sufficiently recent inverter data" },
        { Status::UnconditionalSolarPassthrough, "unconditionally passing through all solar power (MQTT override)" },
        { Status::Stable, "the system is stable, the last power limit is still valid" },
    };

    auto iter = texts.find(status);
    if (iter == texts.end()) { return missing; }

    return iter->second;
}

void PowerLimiterClass::announceStatus(PowerLimiterClass::Status status)
{
    // this method is called with high frequency. print the status text if
    // the status changed since we last printed the text of another one.
    // otherwise repeat the info with a fixed interval.
    if (_lastStatus == status && millis() < _lastStatusPrinted + 10 * 1000) { return; }

    // after announcing once that the DPL is disabled by configuration, it
    // should just be silent while it is disabled.
    if (status == Status::DisabledByConfig && _lastStatus == status) { return; }

    DTU_LOGI("%s", getStatusText(status).data());

    _lastStatus = status;
    _lastStatusPrinted = millis();
}

void PowerLimiterClass::reloadConfig()
{
    auto const& config = Configuration.get();

    if (!config.PowerLimiter.Enabled || Mode::Disabled == _mode) {
        _retirees.insert(
            _retirees.end(),
            std::make_move_iterator(_inverters.begin()),
            std::make_move_iterator(_inverters.end())
        );

        _inverters.clear();

        _reloadConfigFlag = false;
        return;
    }

    auto iter = _inverters.begin();
    while (iter != _inverters.end()) {
        bool stillGoverned = false;

        for (size_t i = 0; i < INV_MAX_COUNT; ++i) {
            auto const& inv = config.PowerLimiter.Inverters[i];
            if (inv.Serial == 0ULL) { break; }
            stillGoverned = inv.Serial == (*iter)->getSerial() && inv.IsGoverned;
            if (stillGoverned) { break; }
        }

        if (!stillGoverned) {
            _retirees.push_back(std::move(*iter));
        }

        iter = _inverters.erase(iter);
    }

    for (size_t i = 0; i < INV_MAX_COUNT; ++i) {
        auto const& invConfig = config.PowerLimiter.Inverters[i];

        if (invConfig.Serial == 0ULL) { break; }

        if (!invConfig.IsGoverned) { continue; }

        auto upInv = PowerLimiterInverter::create(invConfig);
        if (upInv) { _inverters.push_back(std::move(upInv)); }
    }

    calcNextInverterRestart();

    _reloadConfigFlag = false;
}

void PowerLimiterClass::loop()
{
    auto const& config = Configuration.get();

    // we know that the Hoymiles library refuses to send any message to any
    // inverter until the system has valid time information. until then we can
    // do nothing, not even shutdown the inverter.
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 5)) {
        return announceStatus(Status::WaitingForValidTimestamp);
    }

    // take care that the last requested power
    // limits and power states are actually reached
    if (updateInverters()) {
        return announceStatus(Status::InverterCmdPending);
    }

    if (_reloadConfigFlag) {
        reloadConfig();
        return announceStatus(Status::ConfigReload);
    }

    if (!config.PowerLimiter.Enabled) {
        return announceStatus(Status::DisabledByConfig);
    }

    if (Mode::Disabled == _mode) {
        return announceStatus(Status::DisabledByMqtt);
    }

    if (_inverters.empty()) {
        return announceStatus(Status::InverterInvalid);
    }

    uint32_t latestInverterStats = 0;

    for (auto const& upInv : _inverters) {
        // in particular, we don't want to wait for stats from inverters that
        // are not eligible because they are (currently) unreachable. this is
        // fine as we ignore them throughout the DPL loop if they are not eligible.
        if (!upInv->isEligible()) { continue; }

        auto oStatsMillis = upInv->getLatestStatsMillis();
        if (!oStatsMillis) {
            return announceStatus(Status::InverterStatsPending);
        }

        latestInverterStats = std::max(*oStatsMillis, latestInverterStats);
    }

    // note that we can only perform unconditional full solar-passthrough or any
    // calculation at all after surviving the loop above, which ensures that we
    // have inverter stats more recent than their respective last update command
    if (Mode::UnconditionalFullSolarPassthrough == _mode) {
        return unconditionalFullSolarPassthrough();
    }

    // if the power meter is being used, i.e., if its data is valid, we want to
    // wait for a new reading after adjusting the inverter limit. otherwise, we
    // proceed as we will use a fallback limit independent of the power meter.
    // the power meter reading is expected to be at most 2 seconds old when it
    // arrives. this can be the case for readings provided by networked meter
    // readers, where a packet needs to travel through the network for some
    // time after the actual measurement was done by the reader.
    if (PowerMeter.isDataValid() && PowerMeter.getLastUpdate() <= (latestInverterStats + 2000)) {
        return announceStatus(Status::PowerMeterPending);
    }

    // since _lastCalculation and _calculationBackoffMs are initialized to
    // zero, this test is passed the first time the condition is checked.
    // Exception: if the negative export limit is actively exceeded, bypass
    // the backoff so inverter reductions fire on every loop cycle, not once
    // per backoff window (which can be up to 1024 ms when the system was
    // previously stable).
    {
        auto const maxNeg = config.PowerLimiter.MaxNegativePowerMeter;
        bool negExportExceeded = (maxNeg < 0)
            && PowerMeter.isDataValid()
            && (PowerMeter.getPowerTotal() < static_cast<float>(maxNeg));

        if (!negExportExceeded
                && (millis() - _lastCalculation) < _calculationBackoffMs) {
            return announceStatus(Status::Stable);
        }

        if (negExportExceeded) {
            // Reset backoff so the very next cycle also fires immediately.
            _calculationBackoffMs = _calculationBackoffMsDefault;
        }
    }

    auto autoRestartInverters = [this]() -> void {
        if (!_nextInverterRestart.first) { return; } // no automatic restarts

        auto constexpr halfOfAllMillis = std::numeric_limits<uint32_t>::max() / 2;
        auto diff = _nextInverterRestart.second - millis();
        if (diff < halfOfAllMillis) { return; }

        for (auto& upInv : _inverters) {
            if (!upInv->isSolarPowered()) {
                DTU_LOGI("sending restart command to inverter %s", upInv->getSerialStr());
                upInv->restart();
            }
        }

        calcNextInverterRestart();
    };

    autoRestartInverters();

    auto getBatteryState = [this,&config]() -> BatteryState {

        // State machine for the battery
        // Conditions:          we use 'Below Stop Threshold', 'Above Start Threshold', 'Solar-Passthrough', 'Use Battery at night',
        //                      'Night/Day' and 'From which direction did we enter the stop-start zone' to determine the state.
        //
        // states               description
        // --------------------------------------------------------------------------------------------------------------------------------
        // STOP:                we must stop the inverter, because the battery is below the stop threshold
        // NO_DISCHARGE:        we can use the inverter, but we do not allow to discharge the battery, A requirement from 'Solar-Passthrough'
        // DISCHARGE_ALLOWED:   we can use the inverter and we allow discharging of the battery
        // DISCHARGE_NIGHT:     we can use the inverter and we allow discharging of a partial charged battery at night.
        //                      A requirement from 'Use Battery at night'
        //
        // Notes: The combination of 'Use Battery at night' and use of 'voltage thresholds' can leads to oscillation between the states
        // STOP and DISCHARGE_NIGHT. To avoid this problem, we allow only one transmission from STOP to DISCHARGE_NIGHT per night.
        // In case of restart or power-cycle, we accept that the inverter may start discharging at night once again.
        // Start-Up can be tricky, because data from the battery provider may not be available. As fallback we use the not very
        // accurate inverter voltage and this can lead to the wrong state.

        // check if we have a battery powered inverter
        if (!usesBatteryPoweredInverter()) { return BatteryState::STOP; }

        // check the stop condition
        auto day = SunPosition.isDayPeriod();
        if (isStopThresholdReached()) {
            _fromStart = false;
            _oneStopPerNightDone = day ? false : true;
            return BatteryState::STOP;
        }

        // check the start condition
        if (isStartThresholdReached()) {
            _fromStart = true;
            return BatteryState::DISCHARGE_ALLOWED;
        }

        // all of the following conditions mean that we are in the "stop-start zone",
        // and we must use the buffered information 'From which direction did we enter the stop-start zone'.

        // if we come from start we always allow discharging of the battery
        if (_fromStart) { return BatteryState::DISCHARGE_ALLOWED; }

        // if we reach this line we come from stop and have to consider the 'Solar-Passthrough' and the 'Use Battery at night' settings.
        auto solarPassThroughEnabled = isSolarPassThroughEnabled();
        auto isBatteryAlwaysUseAtNightEnabled = config.PowerLimiter.BatteryAlwaysUseAtNight;

        // When `Use Battery at night` is disabled or when its day, battery should not be discharged
        if (!isBatteryAlwaysUseAtNightEnabled || day) {
            _oneStopPerNightDone = false;

            // Only allow inverters to be active if we are in solar pass-through mode.
            // Otherwise we stop the battery inverters.
            if (solarPassThroughEnabled) { return BatteryState::NO_DISCHARGE; }
            return BatteryState::STOP;
         }

        // When `Use Battery at night` is enabled, and its night and we have already stopped the battery once per night, we keep the STOP state.
        // Otherwise we allow discharging of a partially charged battery.
        if (_oneStopPerNightDone) { return BatteryState::STOP; }
        return BatteryState::DISCHARGE_NIGHT;
    };

    auto getFullSolarPassthrough = [this,&config]() -> bool {
        // we only do full solar PT if general solar PT is enabled
        // and we are above the 'battery start threshold'
        if (!isSolarPassThroughEnabled() || !isStartThresholdReached()) { return false; }

        if (testThreshold(config.PowerLimiter.FullSolarPassThroughSoc,
                        config.PowerLimiter.FullSolarPassThroughStartVoltage,
                        [](float a, float b) -> bool { return a >= b; })) {
            return true;
        }

        if (testThreshold(config.PowerLimiter.FullSolarPassThroughSoc,
                        config.PowerLimiter.FullSolarPassThroughStopVoltage,
                        [](float a, float b) -> bool { return a < b; })) {
            return false;
        }

        return _fullSolarPassThroughActive;
    };

    auto getLoadCorrectedVoltage = [this,&config]() -> float {
        // TODO(schlimmchen): use the battery's data if available,
        // i.e., the current drawn from the battery as reported by the battery.
        float acPower = getBatteryInvertersOutputAcWatts();
        float dcVoltage = getBatteryVoltage();

        if (dcVoltage <= 0.0) { return 0.0; }

        return dcVoltage + (acPower * config.PowerLimiter.VoltageLoadCorrectionFactor);
    };

    _loadCorrectedVoltage = getLoadCorrectedVoltage();
    _batteryState = getBatteryState();
    _fullSolarPassThroughActive = getFullSolarPassthrough();

    DTU_LOGD("up %lu s, it is %s, next inverter restart at %d s (set to %d)",
            millis()/1000,
            (SunPosition.isDayPeriod()?"day":"night"),
            _nextInverterRestart.second/1000,
            config.PowerLimiter.RestartHour);

    if (usesBatteryPoweredInverter()) {
        DTU_LOGD("battery interface %sabled, SoC %.1f %% (%s), age %u s (%s)",
                (config.Battery.Enabled?"en":"dis"),
                Battery.getStats()->getSoC(),
                (config.PowerLimiter.IgnoreSoc?"ignored":"used"),
                Battery.getStats()->getSoCAgeSeconds(),
                (Battery.getStats()->isSoCValid()?"valid":"stale"));

        auto dcVoltage = getBatteryVoltage(true/*log voltages only once per DPL loop*/);
        DTU_LOGD("battery voltage %.2f V, load-corrected voltage %.2f V @ %.0f W, factor %.5f 1/A",
                dcVoltage, _loadCorrectedVoltage,
                getBatteryInvertersOutputAcWatts(),
                config.PowerLimiter.VoltageLoadCorrectionFactor);

        DTU_LOGD("battery discharge %s, start %.2f V or %u %%, stop %.2f V or %u %%",
                (((_batteryState == BatteryState::DISCHARGE_ALLOWED) || (_batteryState == BatteryState::DISCHARGE_NIGHT))?"allowed":
                (_batteryState == BatteryState::NO_DISCHARGE)?"restricted":"stopped"),
                config.PowerLimiter.VoltageStartThreshold,
                config.PowerLimiter.BatterySocStartThreshold,
                config.PowerLimiter.VoltageStopThreshold,
                config.PowerLimiter.BatterySocStopThreshold);

        if (isSolarPassThroughEnabled()) {
            DTU_LOGD("full solar-passthrough %s, start %.2f V or %u %%, stop %.2f V",
                    (isFullSolarPassthroughActive()?"active":"dormant"),
                    config.PowerLimiter.FullSolarPassThroughStartVoltage,
                    config.PowerLimiter.FullSolarPassThroughSoc,
                    config.PowerLimiter.FullSolarPassThroughStopVoltage);
        }

        DTU_LOGD("start %sreached, stop %sreached, solar-passthrough %sabled, use at night %sabled and %s",
                (isStartThresholdReached()?"":"NOT "),
                (isStopThresholdReached()?"":"NOT "),
                (isSolarPassThroughEnabled()?"en":"dis"),
                (config.PowerLimiter.BatteryAlwaysUseAtNight?"en":"dis"),
                ((_batteryState == BatteryState::DISCHARGE_NIGHT)?"active":"dormant"));

        DTU_LOGD("total max AC power is %u W, conduction losses are %u %%",
            config.PowerLimiter.TotalUpperPowerLimit,
            config.PowerLimiter.ConductionLosses);
    }

    // Determine whether any inverter has an explicit per-phase assignment.
    // If none do, use the legacy single-pass (Total) path for full backward compat.
    bool hasPhasedInverters = false;
    for (auto const& upInv : _inverters) {
        // ReferencePhase::Total == 0; any other value means a specific phase
        if (static_cast<uint8_t>(upInv->getPhaseAssignment()) != 0u) {
            hasPhasedInverters = true;
            break;
        }
    }

    uint16_t totalCovered = 0;

    // ── Max negative power (export limit) ─────────────────────────────
    // When configured, check the TOTAL meter against the limit.  If it is
    // more negative than allowed, compute the overshoot so that every
    // regulatePhase / calcTargetOutput call can subtract it from its
    // target.  This reduces ALL inverters (every phase) proportionally,
    // preventing excessive grid export.
    // maxNegOvershoot > 0 means the total meter is too negative by that
    // many watts.  0 means no overshoot (or feature disabled).
    // When full solar passthrough is active, the export limit is intentionally
    // bypassed — we want all solar pushed to AC regardless of grid export.
    uint16_t maxNegOvershoot = 0;
    if (!isFullSolarPassthroughActive()) {
        auto maxNeg = config.PowerLimiter.MaxNegativePowerMeter;
        if (maxNeg < 0 && PowerMeter.isDataValid()) {
            auto totalMeter = static_cast<int16_t>(
                PowerMeter.getPowerTotal() + (PowerMeter.getPowerTotal() > 0 ? 0.5f : -0.5f));
            if (totalMeter < maxNeg) {
                maxNegOvershoot = static_cast<uint16_t>(maxNeg - totalMeter);
                DTU_LOGD("max negative limit %d W exceeded (total meter %d W), "
                         "global overshoot %u W — reducing all inverters",
                         maxNeg, totalMeter, maxNegOvershoot);
            }
        }
    }

    if (!hasPhasedInverters) {
        // ── Legacy single-pass (all inverters assigned to Total) ──────────
        uint16_t inverterTotalPower = calcTargetOutput(0);
        // apply max-negative overshoot reduction
        inverterTotalPower = (inverterTotalPower > maxNegOvershoot)
                           ? inverterTotalPower - maxNegOvershoot : 0;
        inverterTotalPower = std::min(inverterTotalPower,
                static_cast<uint16_t>(config.PowerLimiter.TotalUpperPowerLimit));

        auto coveredBySolar = updateInverterLimits(inverterTotalPower, sSolarPoweredFilter, sSolarPoweredExpression);
        auto remainingAfterSolar = (inverterTotalPower >= coveredBySolar) ? inverterTotalPower - coveredBySolar : 0;
        auto coveredBySmartBuffer = updateInverterLimits(remainingAfterSolar, sSmartBufferPoweredFilter, sSmartBufferPoweredExpression);
        auto remainingAfterSmartBuffer = (remainingAfterSolar >= coveredBySmartBuffer) ? remainingAfterSolar - coveredBySmartBuffer : 0;
        auto powerBusUsage = calcPowerBusUsage(remainingAfterSmartBuffer);
        auto coveredByBattery = updateInverterLimits(powerBusUsage, sBatteryPoweredFilter, sBatteryPoweredExpression);

        totalCovered = coveredBySolar + coveredBySmartBuffer + coveredByBattery;
    } else {
        // ── Multi-phase pass ──────────────────────────────────────────────
        // Phase Pass: L1, L2, L3 — each regulated independently.
        // Unmet demand from each phase accumulates in the residual pool.
        //
        // The DC bus budget (solar passthrough + battery discharge
        // allowance) is a SHARED resource.  Compute it once and pass
        // it through every regulatePhase() call so that battery-
        // powered inverters across all phases collectively never
        // exceed the real limit.
        uint16_t dcBusBudgetAc = calcPowerBusUsage(UINT16_MAX);
        uint16_t const dcBusBudgetAcInitial = dcBusBudgetAc; // pool: track consumption across phases
        uint16_t globalAllowanceAc = config.PowerLimiter.TotalUpperPowerLimit;

        uint16_t overshootByAssignment[4] = { 0, 0, 0, 0 };
        if (maxNegOvershoot > 0) {
            uint32_t outputByAssignment[4] = { 0, 0, 0, 0 };
            uint32_t totalOutputForOvershoot = 0;

            for (auto const& upInv : _inverters) {
                if (!upInv->isEligible()) { continue; }

                auto assignment = static_cast<uint8_t>(upInv->getPhaseAssignment());
                if (assignment > 3u) { continue; }

                auto output = upInv->getCurrentOutputAcWatts();
                outputByAssignment[assignment] += output;
                totalOutputForOvershoot += output;
            }

            if (totalOutputForOvershoot > 0) {
                uint32_t assignedOvershoot = 0;
                uint8_t highestOutputAssignment = 0;

                for (uint8_t assignment = 0; assignment < 4; ++assignment) {
                    if (outputByAssignment[assignment] > outputByAssignment[highestOutputAssignment]) {
                        highestOutputAssignment = assignment;
                    }

                    auto weightedOvershoot = static_cast<uint32_t>(maxNegOvershoot) * outputByAssignment[assignment];
                    overshootByAssignment[assignment] = static_cast<uint16_t>(weightedOvershoot / totalOutputForOvershoot);
                    assignedOvershoot += overshootByAssignment[assignment];
                }

                if (assignedOvershoot < maxNegOvershoot) {
                    overshootByAssignment[highestOutputAssignment] += (maxNegOvershoot - assignedOvershoot);
                }

                DTU_LOGD("weighted overshoot distribution total=%u W: Total=%u W, L1=%u W, L2=%u W, L3=%u W",
                         maxNegOvershoot,
                         overshootByAssignment[0],
                         overshootByAssignment[1],
                         overshootByAssignment[2],
                         overshootByAssignment[3]);
            }
        }

        DTU_LOGD("global DC bus budget: %u W AC, global allowance: %u W AC",
                 dcBusBudgetAc, globalAllowanceAc);

        int16_t residualL1 = 0, residualL2 = 0, residualL3 = 0;
        totalCovered += regulatePhase(1, residualL1, dcBusBudgetAc, globalAllowanceAc, overshootByAssignment[1]);
        totalCovered += regulatePhase(2, residualL2, dcBusBudgetAc, globalAllowanceAc, overshootByAssignment[2]);
        totalCovered += regulatePhase(3, residualL3, dcBusBudgetAc, globalAllowanceAc, overshootByAssignment[3]);

        // Residual Pass: collect all unmet demand and redistribute to
        // Total-assigned inverters (and phase-assigned inverters that still
        // have headroom, since regulatePhase(0) uses all inverters).
        int32_t residualPool = static_cast<int32_t>(residualL1)
                             + static_cast<int32_t>(residualL2)
                             + static_cast<int32_t>(residualL3);

        DTU_LOGD("residual pool: %d W (L1:%d L2:%d L3:%d)",
                 residualPool, residualL1, residualL2, residualL3);

        // Check if any phase-assigned inverter has a pending limit change
        // scheduled by the regulatePhase(L1/L2/L3) calls above. If so, skip
        // regulating Total inverters this cycle — wait for the phase changes
        // to be applied and reflected in the meter before adjusting Total,
        // to avoid overshoot oscillation.
        bool phaseInverterPending = false;
        for (auto const& upInv : _inverters) {
            if (static_cast<uint8_t>(upInv->getPhaseAssignment()) == 0u) { continue; }
            if (upInv->hasTargetLimitPending()) {
                phaseInverterPending = true;
                break;
            }
        }

        uint16_t totalAssignedCurrentOutput = 0;
        for (auto const& upInv : _inverters) {
            if (static_cast<uint8_t>(upInv->getPhaseAssignment()) != 0u) { continue; }
            if (!upInv->isEligible()) { continue; }

            totalAssignedCurrentOutput += upInv->getCurrentOutputAcWatts();
        }

        uint16_t totalRegulationTarget = calcTargetOutput(0);
        if (overshootByAssignment[0] > 0) {
            totalRegulationTarget = (totalRegulationTarget > overshootByAssignment[0])
                                  ? totalRegulationTarget - overshootByAssignment[0] : 0;
        }
        totalRegulationTarget = std::min(totalRegulationTarget, globalAllowanceAc);

        bool totalNeedsDecrease = totalAssignedCurrentOutput > totalRegulationTarget;

        // Battery STOP state must not be deferred — Total-assigned battery
        // inverters need to be shut down immediately regardless of pending
        // phase changes, otherwise the deferral keeps them running
        // indefinitely while phase standby commands keep retriggering.
        if (_batteryState == BatteryState::STOP) {
            bool hasTotalBatteryInverter = false;
            for (auto const& upInv : _inverters) {
                if (static_cast<uint8_t>(upInv->getPhaseAssignment()) != 0u) { continue; }
                if (upInv->isBatteryPowered() && upInv->isProducing()) {
                    hasTotalBatteryInverter = true;
                    break;
                }
            }
            if (hasTotalBatteryInverter) { totalNeedsDecrease = true; }
        }

        if (phaseInverterPending
            && !totalNeedsDecrease
            && !isFullSolarPassthroughActive()) {
            DTU_LOGD("phase inverters have pending limit changes, "
                     "deferring Total regulation to next cycle");
        } else {
            // Regulate Total-assigned inverters only when phase inverters are
            // settled, so the meter readings reflect their actual output.
            // But if Total already needs to ramp down, do that immediately so
            // export limiting is not blocked by pending phase updates.
            int16_t unused;
            totalCovered += regulatePhase(0, unused, dcBusBudgetAc, globalAllowanceAc, overshootByAssignment[0]);
        }

        // Full solar passthrough: push all available solar through battery
        // inverters, even beyond per-phase demand — mirrors the legacy single-
        // phase path. We know how much the phase passes already consumed from
        // the pool (dcBusBudgetAcInitial - dcBusBudgetAc). If there is still
        // un-pushed solar (dcBusBudgetAc > 0 after phases), override all
        // battery inverters to the full solar target so the remainder is used.
        if (isFullSolarPassthroughActive()) {
            auto solarDc = getSolarPassthroughPower();
            auto solarAc = dcPowerBusToInverterAc(solarDc);
            uint16_t usedByPhases = (dcBusBudgetAcInitial > dcBusBudgetAc)
                                  ? dcBusBudgetAcInitial - dcBusBudgetAc : 0;
            // Also cap by global allowance remaining.
            // FSP pushes at least all available solar through, but if demand
            // exceeds solar (and battery discharge is allowed), also serve the
            // excess from the battery. Use the larger of:
            //  - totalRegulationTarget (Total-assigned inverter need), and
            //  - residualPool (per-phase unmet demand skipped in phase passes)
            // to avoid double-counting demand by summing both values.
            uint16_t demandTarget = 0;
            {
                uint16_t residualDemand = 0;
                if (residualPool > 0) {
                    residualDemand = static_cast<uint16_t>(
                        std::min<int32_t>(residualPool, globalAllowanceAc));
                }
                demandTarget = std::max(totalRegulationTarget, residualDemand);
                demandTarget = std::min(demandTarget, globalAllowanceAc);
            }
            auto fspTarget = std::min(
                std::max(solarAc, demandTarget),
                globalAllowanceAc);
            DTU_LOGD("full solar-passthrough (multi-phase): solar %u W AC, "
                     "pool initial %u W, used by phases %u W, remaining %u W, "
                     "global allowance remaining %u W, demand %u W, fsp target %u W",
                     solarAc, dcBusBudgetAcInitial, usedByPhases, dcBusBudgetAc,
                     globalAllowanceAc, demandTarget, fspTarget);
            if (dcBusBudgetAc > 0 && fspTarget > 0) {
                // Push all battery inverters up to fspTarget collectively
                // (at least solar, more if demand exceeds solar).
                updateInverterLimits(fspTarget, sBatteryPoweredFilter,
                    std::string(sBatteryPoweredExpression) + "/full-solar-pt");
            }
        }
    }

    for (auto const &upInv : _inverters) { upInv->debug(); }

    _lastExpectedInverterOutput = totalCovered;

    bool limitUpdated = updateInverters();

    _lastCalculation = millis();

    if (!limitUpdated) {
        // Increase polling backoff only when the export limit is not actively
        // exceeded.  If we are still over the limit, keep the fast cycle.
        auto const maxNeg = config.PowerLimiter.MaxNegativePowerMeter;
        bool negExportExceeded = (maxNeg < 0)
            && PowerMeter.isDataValid()
            && !isFullSolarPassthroughActive()
            && (PowerMeter.getPowerTotal() < static_cast<float>(maxNeg));

        if (!negExportExceeded) {
            _calculationBackoffMs = std::min<uint32_t>(1024, _calculationBackoffMs * 2);
        }
        return announceStatus(Status::Stable);
    }

    _calculationBackoffMs = _calculationBackoffMsDefault;
}

std::pair<float, char const*> PowerLimiterClass::getInverterDcVoltage() const
{
    auto const& config = Configuration.get();

    auto iter = _inverters.cbegin();
    while(iter != _inverters.cend()) {
        if ((*iter)->getSerial() == config.PowerLimiter.InverterSerialForDcVoltage) {
            break;
        }
        ++iter;
    }

    auto voltage = -1.0;

    if (iter == _inverters.cend()) {
        return { voltage, "<unknown>" };
    }

    if ((*iter)->isReachable()) {
        voltage = (*iter)->getDcVoltage(config.PowerLimiter.InverterChannelIdForDcVoltage);
    }

    return { voltage, (*iter)->getSerialStr() };
}

/**
 * determines the battery's voltage, trying multiple data providers. the most
 * accurate data is expected to be delivered by a BMS, if it's available. more
 * accurate and more recent than the inverter's voltage reading is the volage
 * at the charge controller's output, if it's available. only as a fallback
 * the voltage reported by the inverter is used.
 */
float PowerLimiterClass::getBatteryVoltage(bool log) const {
    auto const& config = Configuration.get();

    float res = 0;

    auto inverter = getInverterDcVoltage();
    if (inverter.first > 0) { res = inverter.first; }

    float chargeControllerVoltage = -1;

    auto chargerOutputVoltage = SolarCharger.getStats()->getOutputVoltage();
    if (chargerOutputVoltage) {
        res = chargeControllerVoltage = *chargerOutputVoltage;
    }

    float bmsVoltage = -1;
    auto stats = Battery.getStats();
    if (config.Battery.Enabled
            && stats->isVoltageValid()
            && stats->getVoltageAgeSeconds() < 60) {
        res = bmsVoltage = stats->getVoltage();
    }

    if (log) {
        DTU_LOGD("BMS: %.2f V, MPPT: %.2f V, inverter %s: %.2f",
                bmsVoltage, chargeControllerVoltage, inverter.second, inverter.first);
    }

    return res;
}

/**
 * calculate the AC output power (limit) to set, such that the inverter uses
 * the given power on its DC side, i.e., adjust the power for the inverter's
 * efficiency.
 */
uint16_t PowerLimiterClass::dcPowerBusToInverterAc(uint16_t dcPower) const
{
    // account for losses between power bus and inverter (cables, junctions...)
    auto const& config = Configuration.get();
    float lossesFactor = 1.00 - static_cast<float>(config.PowerLimiter.ConductionLosses)/100;

    // we cannot know the efficiency at the new limit. even if we could we
    // cannot know which inverter is assigned which limit. hence we use a
    // reasonable, conservative, fixed inverter efficiency.
    return 0.95 * lossesFactor * dcPower;
}

/**
 * implements the "uncoditional full solar passthrough" mode of operation. in this mode of
 * operation, the inverters shall behave as if they were connected to the solar
 * panels directly, i.e., all solar power (and only solar power) is converted
 * to AC power, independent from the power meter reading.
 */
void PowerLimiterClass::unconditionalFullSolarPassthrough()
{
    auto now = millis();
    if ((now - _lastCalculation) < _calculationBackoffMs) { return; }
    _lastCalculation = now;

    for (auto const& upInv : _inverters) {
        if (!upInv->isEligible()) { continue; }
        if (!upInv->isBatteryPowered()) { upInv->setMaxOutput(); }
    }

    uint16_t targetOutput = 0;

    auto solarChargerOutput = SolarCharger.getStats()->getOutputPowerWatts();
    if (solarChargerOutput) {
        targetOutput = static_cast<uint16_t>(std::max<int32_t>(0, *solarChargerOutput));
        targetOutput = dcPowerBusToInverterAc(targetOutput);
    }

    _calculationBackoffMs = 1 * 1000;
    updateInverterLimits(targetOutput, sBatteryPoweredFilter, sBatteryPoweredExpression);
    return announceStatus(Status::UnconditionalSolarPassthrough);
}

uint8_t PowerLimiterClass::getInverterUpdateTimeouts() const
{
    uint8_t res = 0;
    for (auto const& upInv : _inverters) {
        res += upInv->getUpdateTimeouts();
    }
    return res;
}

uint8_t PowerLimiterClass::getPowerLimiterState() const
{
    bool reachable = false;
    bool producing = false;
    for (auto const& upInv : _inverters) {
        reachable |= upInv->isReachable();
        producing |= upInv->isProducing();
    }

    if (!reachable) {
        return PL_UI_STATE_INACTIVE;
    }

    if (!producing) {
        return PL_UI_STATE_CHARGING;
    }

    return ((_batteryState == BatteryState::DISCHARGE_ALLOWED || _batteryState == BatteryState::DISCHARGE_NIGHT))
        ? PL_UI_STATE_USE_SOLAR_AND_BATTERY : PL_UI_STATE_USE_SOLAR_ONLY;
}

uint16_t PowerLimiterClass::calcTargetOutput(uint8_t phase) const
{
    auto const& config = Configuration.get();
    auto targetConsumption = config.PowerLimiter.TargetPowerConsumption;
    auto baseLoad = config.PowerLimiter.BaseLoadLimit;

    auto meterValid = PowerMeter.isDataValid();
    float meterValue = getMeterValueForPhase(phase);

    DTU_LOGD("phase %u: targeting %d W, base load is %u W, power meter reads %.1f W (%s)",
            phase, targetConsumption, baseLoad, meterValue,
            (meterValid?"valid":"stale"));

    if (!meterValid) { return baseLoad; }

    // the desired total output of all eligible inverters is whatever they are
    // producing right now plus the difference between the target consumption
    // and the power meter reading
    auto roundedMeterValue = static_cast<int16_t>(meterValue + (meterValue > 0 ? 0.5 : -0.5));

    // For per-phase regulation: Total-assigned inverters that physically feed
    // into this phase push the phase meter reading negative, but they are
    // managed by the Total regulation pass, not by this per-phase pass.
    // Add their output back so we see the "net consumption" on this phase
    // without the Total inverter's contribution.
    if (phase != 0) {
        for (auto const& upInv : _inverters) {
            auto pa = static_cast<uint8_t>(upInv->getPhaseAssignment());
            if (pa != 0) { continue; } // only Total-assigned inverters

            auto cp = static_cast<uint8_t>(upInv->getConnectedPhase());
            if (cp != phase) { continue; } // only those feeding into this phase

            roundedMeterValue += upInv->getCurrentOutputAcWatts();
            DTU_LOGD("phase %u: compensating Total inverter %s output %d W on this phase",
                    phase, upInv->getSerialStr(), upInv->getCurrentOutputAcWatts());
        }
    }

    // we have to correct the meter reading if there are inverters connected to
    // AC between the grid (billing meter) and OpenDTU-OnBattery's power meter.
    // For per-phase regulation we only correct using inverters assigned to this
    // phase (or all inverters for the Total pass).
    for (auto const& upInv : _inverters) {
        if (upInv->isBehindPowerMeter()) { continue; }

        // phase-aware: only subtract inverters on this phase, or all for Total
        if (phase != 0) {
            auto phaseAssignment = static_cast<uint8_t>(upInv->getPhaseAssignment());
            if (phaseAssignment != phase) { continue; }
        }

        // it is to be expected that solar-powered inverters are unreachable
        // during the night, in which case we don't want to account for their
        // last reported AC output, as they are not producing power.
        auto isDayPeriod = SunPosition.isDayPeriod();
        if (upInv->isSolarPowered() && !upInv->isReachable() && !isDayPeriod) { continue; }

        // in all other cases, even for unreachable inverters, we assume that
        // they still produce the amount of AC output that they last reported.
        // if we assumed unreachable inverters are not producing, we will
        // potentially produce way too much power. as information is missing
        // that could make sure we do the right thing, we have to make an
        // assumption about unreachable inverters.
        roundedMeterValue -= upInv->getCurrentOutputAcWatts();
    }

    int16_t currentTotalOutput = 0;
    for (auto const& upInv : _inverters) {
        // non-eligible inverters don't participate in this DPL round at all.
        // inverters in standby report 0 W output, so we can iterate them.
        if (!upInv->isEligible()) { continue; }

        // phase-aware: only count output of inverters assigned to this phase.
        // For phase=0 (Total pass) this means only Total-assigned inverters —
        // including ALL phases would inflate the target far beyond what the
        // Total-assigned inverters can physically produce, causing them to stay
        // pegged at their hardware maximum permanently.
        auto phaseAssignment = static_cast<uint8_t>(upInv->getPhaseAssignment());
        if (phaseAssignment != phase) { continue; }

        // Use the expected (commanded) output rather than the actual reported
        // AC output.  After a limit command is sent and ACK'd, the inverter's
        // actual power statistics lag by several seconds.  If we use the stale
        // actual value here, calcTargetOutput() underestimates currentTotalOutput
        // while the meter has already reacted to the new power level, causing
        // DPL to immediately issue another increase command and oscillate.
        currentTotalOutput += upInv->getExpectedOutputAcWatts();
    }

    // this value is negative if we are exporting more than "targetConsumption"
    // power to the grid using generators other than DPL-governed inverters.
    int16_t targetOutput = currentTotalOutput + roundedMeterValue - targetConsumption;

    // if we are already exporting more power than the (negative) target
    // consumption value allows us to, we don't want DPL-governed inverters to
    // produce any power at all.
    if (targetOutput < 0) { return 0; }

    return static_cast<uint16_t>(targetOutput);
}

float PowerLimiterClass::getMeterValueForPhase(uint8_t phase) const
{
    if (phase == 0) { return PowerMeter.getPowerTotal(); }
    auto oPhase = PowerMeter.getPowerPhase(phase);
    // fall back to total if this provider doesn't supply per-phase data
    return oPhase.value_or(PowerMeter.getPowerTotal());
}

/**
 * Run one full regulation pass for the given phase (1=L1,2=L2,3=L3) or for
 * the residual/total pool (phase=0).  The Solar→SmartBuffer→Battery cascade
 * is applied only to inverters whose PhaseAssignment matches `phase`.
 * For phase=0 (residual pass) ALL inverters participate regardless of
 * PhaseAssignment so that any leftover demand can be absorbed.
 *
 * Returns the total watts covered.  `residual` is set to unmet demand
 * (positive = demand not covered, negative = over-supplied).
 */
uint16_t PowerLimiterClass::regulatePhase(uint8_t phase, int16_t& residual,
                                          uint16_t& dcBusBudgetRemainingAc,
                                          uint16_t& globalAllowanceRemainingAc,
                                          uint16_t maxNegOvershoot)
{
    // Build a phase filter that layers on top of the power-source filter.
    // For phase=0 (Total/residual) only Total-assigned inverters are eligible;
    // per-phase inverters were already regulated in their own pass and must
    // not have their limits overwritten here.
    auto phaseFilter = [phase](PowerLimiterInverter const& inv) -> bool {
        return static_cast<uint8_t>(inv.getPhaseAssignment()) == phase;
    };

    auto solarPhaseFilter = [&](PowerLimiterInverter const& inv) {
        return sSolarPoweredFilter(inv) && phaseFilter(inv);
    };
    auto smartBufferPhaseFilter = [&](PowerLimiterInverter const& inv) {
        return sSmartBufferPoweredFilter(inv) && phaseFilter(inv);
    };
    auto batteryPhaseFilter = [&](PowerLimiterInverter const& inv) {
        return sBatteryPoweredFilter(inv) && phaseFilter(inv);
    };

    std::string phaseExpr = (phase == 0) ? "residual/total" :
                            (phase == 1) ? "L1" :
                            (phase == 2) ? "L2" : "L3";

    uint16_t inverterTotalPower = calcTargetOutput(phase);

    // Apply the global max-negative overshoot reduction so that ALL phases
    // (and Total) collectively ramp down when the total meter is too negative.
    if (maxNegOvershoot > 0) {
        auto before = inverterTotalPower;
        inverterTotalPower = (inverterTotalPower > maxNegOvershoot)
                           ? inverterTotalPower - maxNegOvershoot : 0;
        DTU_LOGD("phase %u: max-neg reduction %u W -> %u W (overshoot %u W)",
                 phase, before, inverterTotalPower, maxNegOvershoot);
    }

    // Cap by the shared global allowance remaining (enforces TotalUpperPowerLimit
    // across all phases collectively, not just per-phase).
    inverterTotalPower = std::min(inverterTotalPower, globalAllowanceRemainingAc);

    auto coveredBySolar = updateInverterLimits(inverterTotalPower,
            solarPhaseFilter, sSolarPoweredExpression + std::string("/") + phaseExpr);
    auto remainingAfterSolar = (inverterTotalPower >= coveredBySolar)
                                 ? inverterTotalPower - coveredBySolar : 0;

    auto coveredBySmartBuffer = updateInverterLimits(remainingAfterSolar,
            smartBufferPhaseFilter, sSmartBufferPoweredExpression + std::string("/") + phaseExpr);
    auto remainingAfterSmartBuffer = (remainingAfterSolar >= coveredBySmartBuffer)
                                       ? remainingAfterSolar - coveredBySmartBuffer : 0;

        uint16_t coveredByBattery = 0;
        if (!isFullSolarPassthroughActive()) {
        // Cap battery usage by remaining DC bus budget so that the total
        // battery-powered output across all phases never exceeds the real
        // solar-passthrough + battery-discharge allowance.
        auto powerBusUsage = std::min(static_cast<uint16_t>(remainingAfterSmartBuffer),
                          dcBusBudgetRemainingAc);
        coveredByBattery = updateInverterLimits(powerBusUsage,
            batteryPhaseFilter, sBatteryPoweredExpression + std::string("/") + phaseExpr);
        dcBusBudgetRemainingAc -= std::min(dcBusBudgetRemainingAc,
                           static_cast<uint16_t>(coveredByBattery));
        }

    uint16_t covered = coveredBySolar + coveredBySmartBuffer + coveredByBattery;
    residual = static_cast<int16_t>(inverterTotalPower) - static_cast<int16_t>(covered);

    // Consume from the global allowance pool.
    globalAllowanceRemainingAc -= std::min(globalAllowanceRemainingAc,
                                           static_cast<uint16_t>(covered));

    DTU_LOGD("phase %u: target %u W, covered %u W, residual %d W, global allowance remaining %u W",
             phase, inverterTotalPower, covered, residual, globalAllowanceRemainingAc);

    return covered;
}

/**
 * assigns new limits to all inverters matching the filter. returns the total
 * amount of power these inverters are expected to produce after the new limits
 * were applied.
 */
uint16_t PowerLimiterClass::updateInverterLimits(uint16_t powerRequested,
        PowerLimiterClass::inverter_filter_t filter, std::string const& filterExpression)
{
    std::vector<PowerLimiterInverter*> matchingInverters;
    uint16_t producing = 0; // sum of AC power the matching inverters produce now

    for (auto& upInv : _inverters) {
        if (!filter(*upInv)) { continue; }

        if (!upInv->isEligible()) { continue; }

        producing += upInv->getCurrentOutputAcWatts();
        matchingInverters.push_back(upInv.get());
    }

    if (matchingInverters.empty()) { return 0; }

    // if we update battery-powered inverters and the battery is in the STOP state,
    // we must put all battery-powered inverters into standby mode,
    // regardless of whether the standby option is enabled or not.
    if ((matchingInverters[0]->isBatteryPowered()) && (_batteryState == BatteryState::STOP)) {
        for (auto pInv : matchingInverters) { pInv->standby(); }
        DTU_LOGD("battery is in STOP state, all battery-powered inverters are put into standby.");
        return 0;
    }

    int32_t diff = powerRequested - producing;

    auto const& config = Configuration.get();
    uint16_t hysteresis = config.PowerLimiter.TargetPowerConsumptionHysteresis;

    bool plural = matchingInverters.size() != 1;
    DTU_LOGD("requesting %d W from %d %s inverter%s currently "
            "producing %d W (diff %i W, hysteresis %d W)",
            powerRequested, matchingInverters.size(), filterExpression.c_str(),
            (plural?"s":""), producing, diff, hysteresis);

    // if 0 W are requested, we set hysteresis to 0 to basically ignore it
    // which allows battery-powered inverters to go into standby and avoid
    // that the battery gets fully discharged.
    if (powerRequested == 0) {
        hysteresis = 0;
    }

    // Suppress hysteresis for increases when powerRequested equals the DC bus
    // budget cap (i.e. the solar passthrough limit).  Without this, an inverter
    // at e.g. 79 W with a solar budget of 116 W would have diff=37 W, which is
    // below typical hysteresis (40 W), permanently blocking it from reaching
    // the available solar power.
    if (diff > 0 && powerRequested == producing + static_cast<uint16_t>(diff)) {
        // Check if every matching inverter is already at or above powerRequested;
        // if not, and the requested amount is the hard budget cap, waive hysteresis.
        // We detect the budget-cap case conservatively: if the increase is smaller
        // than hysteresis but there are no solar/battery inverters that could
        // provide more than powerRequested, treat hysteresis as 0.
        bool allAtMax = true;
        for (auto const pInv : matchingInverters) {
            if (pInv->getMaxIncreaseWatts() > 0) { allAtMax = false; break; }
        }
        if (!allAtMax) { hysteresis = std::min(hysteresis, static_cast<uint16_t>(diff)); }
    }

    if (std::abs(diff) < static_cast<int32_t>(hysteresis)) { return producing; }

    uint16_t covered = 0;

    if (diff < 0) {
        uint16_t reduction = static_cast<uint16_t>(diff * -1);

        uint16_t totalMaxReduction = 0;
        for (auto const pInv : matchingInverters) {
            totalMaxReduction += pInv->getMaxReductionWatts(false/*no standby*/);
        }

        // test whether we need to put at least one of the inverters into
        // standby to achieve the requested reduction.
        bool allowStandby = (totalMaxReduction < reduction);

        std::sort(matchingInverters.begin(), matchingInverters.end(),
                [allowStandby](auto const a, auto const b) {
                    // When standby is needed, prefer to standby phase-assigned
                    // inverters first and keep Total-assigned ones running.
                    // Phase assignment 0 = Total (highest priority to keep).
                    if (allowStandby) {
                        auto aIsTotal = (static_cast<uint8_t>(a->getPhaseAssignment()) == 0);
                        auto bIsTotal = (static_cast<uint8_t>(b->getPhaseAssignment()) == 0);
                        if (aIsTotal != bIsTotal) {
                            // phase-assigned (non-Total) sorts first → gets reduced/standby first
                            return !aIsTotal;
                        }
                    }
                    auto aReduction = a->getMaxReductionWatts(allowStandby);
                    auto bReduction = b->getMaxReductionWatts(allowStandby);
                    return aReduction > bReduction;
                });

        for (auto pInv : matchingInverters) {
            auto maxReduction = pInv->getMaxReductionWatts(allowStandby);
            if (reduction >= hysteresis && maxReduction >= hysteresis) {
                reduction -= pInv->applyReduction(reduction, allowStandby);
            }
            covered += pInv->getExpectedOutputAcWatts();
        }
    }
    else {
        uint16_t increase = static_cast<uint16_t>(diff);

        std::sort(matchingInverters.begin(), matchingInverters.end(),
                [](auto const a, auto const b) {
                    return a->getMaxIncreaseWatts() > b->getMaxIncreaseWatts();
                });

        for (auto pInv : matchingInverters) {
            auto maxIncrease = pInv->getMaxIncreaseWatts();
            if (increase >= hysteresis && maxIncrease >= hysteresis) {
                increase -= pInv->applyIncrease(increase);
            }
            covered += pInv->getExpectedOutputAcWatts();
        }
    }

    DTU_LOGD("will cover %d W using %d %s inverter%s",
            covered, matchingInverters.size(),
            filterExpression.c_str(), (plural?"s":""));

    return covered;
}

// calculates how much power the battery-powered inverters shall draw from the
// power bus, which we call the part of the circuitry that is supplied by the
// solar charge controller(s), possibly an AC charger, as well as the battery.
uint16_t PowerLimiterClass::calcPowerBusUsage(uint16_t powerRequested) const
{
    // We check if the PSU is on and disable battery-powered inverters in this
    // case. The PSU should reduce power or shut down first before the
    // battery-powered inverters kick in. The only case where this is not
    // desired is if the battery is over the Full Solar Passthrough Threshold.
    // In this case battery-powered inverters should produce power and the PSU
    // will shut down as a consequence.
    if (!isFullSolarPassthroughActive() && GridCharger.getAutoPowerStatus()) {
        DTU_LOGD("DC power bus usage blocked by GridCharger auto power");
        return 0;
    }

    if (Battery.getStats()->getImmediateChargingRequest()) {
        DTU_LOGD("DC power bus usage blocked by immediate charging request");
        return 0;
    }

    if (_batteryState == BatteryState::STOP) {
        DTU_LOGD("DC power bus usage blocked by battery below the stop threshold");
        return 0;
    }

    auto solarOutputDc = getSolarPassthroughPower();
    auto solarOutputAc = dcPowerBusToInverterAc(solarOutputDc);
    if (isFullSolarPassthroughActive() && solarOutputAc > powerRequested) {
        DTU_LOGD("using %u/%u W DC/AC from DC power bus (full solar-passthrough)",
                solarOutputDc, solarOutputAc);

        return solarOutputAc;
    }

    auto oBatteryDischargeLimit = getBatteryDischargeLimit();
    if (!oBatteryDischargeLimit) {
        DTU_LOGD("granting %d W from DC power bus (no battery discharge "
                "limit), solar power is %u/%u W DC/AC",
                powerRequested, solarOutputDc, solarOutputAc);
        return powerRequested;
    }

    auto batteryAllowanceAc = dcPowerBusToInverterAc(*oBatteryDischargeLimit);

    DTU_LOGD("battery allowance is %u/%u W DC/AC, solar power is %u/%u W DC/AC, "
            "requested are %u W AC",
            *oBatteryDischargeLimit, batteryAllowanceAc,
            solarOutputDc, solarOutputAc, powerRequested);

    uint16_t allowance = batteryAllowanceAc + solarOutputAc;
    return std::min(powerRequested, allowance);
}

bool PowerLimiterClass::updateInverters()
{
    bool busy = false;

    for (auto& upInv : _inverters) {
        if (upInv->update()) { busy = true; }
    }

    auto iter = _retirees.begin();
    while (iter != _retirees.end()) {
        if ((*iter)->retire()) {
            busy = true;
            ++iter;
            continue;
        }

        iter = _retirees.erase(iter);
    }

    return busy;
}

uint16_t PowerLimiterClass::getSolarPassthroughPower() const
{
    if (!isSolarPassThroughEnabled() || isBelowStopThreshold()) {
        return 0;
    }

    std::optional<float> oSolarChargerOutput = SolarCharger.getStats()->getOutputPowerWatts();

    // This value can be negative if a charge controller with a load output is used
    // and the load is consuming more power than the charge controller is producing.
    return std::max<float>(0, oSolarChargerOutput.value_or(0));
}

float PowerLimiterClass::getBatteryInvertersOutputAcWatts() const
{
    float res = 0;

    for (auto const& upInv : _inverters) {
        if (!upInv->isBatteryPowered()) { continue; }
        // TODO(schlimmchen): we must use the DC power instead, as the battery
        // voltage drops proportional to the DC current draw, but the AC power
        // output does not correlate with the battery current or voltage.
        res += upInv->getCurrentOutputAcWatts();
    }

    return res;
}

std::optional<uint16_t> PowerLimiterClass::getBatteryDischargeLimit() const
{
    if ((_batteryState == BatteryState::STOP) || (_batteryState == BatteryState::NO_DISCHARGE)) { return 0; }

    auto currentLimit = Battery.getDischargeCurrentLimit();
    if (currentLimit == FLT_MAX) { return std::nullopt; }

    if (currentLimit <= 0) { currentLimit = -currentLimit; }

    // this uses inverter voltage since there is a voltage drop between
    // battery and inverter, so since we are regulating the inverter
    // power we should use its voltage.
    auto inverter = getInverterDcVoltage();
    if (inverter.first <= 0) {
        DTU_LOGE("could not determine inverter voltage");
        return 0;
    }

    return inverter.first * currentLimit;
}

bool PowerLimiterClass::testThreshold(float socThreshold, float voltThreshold,
        std::function<bool(float, float)> compare) const
{
    auto const& config = Configuration.get();

    // prefer SoC provided through battery interface, unless disabled by user
    auto stats = Battery.getStats();
    if (!config.PowerLimiter.IgnoreSoc
            && config.Battery.Enabled
            && socThreshold > 0.0
            && stats->isSoCValid()
            && stats->getSoCAgeSeconds() < 60) {
              return compare(stats->getSoC(), socThreshold);
    }

    // use voltage threshold as fallback
    if (voltThreshold <= 0.0) { return false; }

    return compare(_loadCorrectedVoltage, voltThreshold);
}

bool PowerLimiterClass::isStartThresholdReached() const
{
    auto const& config = Configuration.get();

    return testThreshold(
            config.PowerLimiter.BatterySocStartThreshold,
            config.PowerLimiter.VoltageStartThreshold,
            [](float a, float b) -> bool { return a >= b; }
    );
}

bool PowerLimiterClass::isStopThresholdReached() const
{
    auto const& config = Configuration.get();

    return testThreshold(
            config.PowerLimiter.BatterySocStopThreshold,
            config.PowerLimiter.VoltageStopThreshold,
            [](float a, float b) -> bool { return a <= b; }
    );
}

bool PowerLimiterClass::isBelowStopThreshold() const
{
    auto const& config = Configuration.get();

    return testThreshold(
            config.PowerLimiter.BatterySocStopThreshold,
            config.PowerLimiter.VoltageStopThreshold,
            [](float a, float b) -> bool { return a < b; }
    );
}

void PowerLimiterClass::calcNextInverterRestart()
{
    if (!usesBatteryPoweredInverter() && !usesSmartBufferPoweredInverter()) {
        _nextInverterRestart = { false, 0 };
        DTU_LOGD("automatic inverter restart disabled");
        return;
    }

    auto const& config = Configuration.get();
    struct tm timeinfo;
    getLocalTime(&timeinfo, 5); // always succeeds as we call this method only
                                // from the DPL loop *after* we already made
                                // sure that time information is available.

    // calculation first step is offset to next restart in minutes
    uint16_t dayMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    uint16_t targetMinutes = config.PowerLimiter.RestartHour * 60;
    uint32_t restartMillis = 0;
    if (config.PowerLimiter.RestartHour > timeinfo.tm_hour) {
        // next restart is on the same day
        restartMillis = targetMinutes - dayMinutes;
    } else {
        // next restart is on next day
        restartMillis = 1440 - dayMinutes + targetMinutes;
    }

    DTU_LOGD("Localtime read %02d:%02d / configured RestartHour %d",
            timeinfo.tm_hour, timeinfo.tm_min, config.PowerLimiter.RestartHour);
    DTU_LOGD("dayMinutes %d / targetMinutes %d", dayMinutes, targetMinutes);
    DTU_LOGD("next inverter restart in %d minutes", restartMillis);

    // convert unit for next restart to milliseconds and add current uptime
    restartMillis *= 60000;
    restartMillis += millis();

    DTU_LOGI("next inverter restart @ %d millis", restartMillis);

    _nextInverterRestart = { true, restartMillis };
}

bool PowerLimiterClass::isSolarPassThroughEnabled() const
{
    auto const& config = Configuration.get();

    // solar passthrough only applies to setups with battery-powered inverters
    if (!usesBatteryPoweredInverter()) { return false; }

    // solarcharger is needed for solar passthrough
    if (!config.SolarCharger.Enabled) { return false; }

    return config.PowerLimiter.SolarPassThroughEnabled;
}

bool PowerLimiterClass::usesBatteryPoweredInverter() const
{
    for (auto const& upInv : _inverters) {
        if (upInv->isBatteryPowered()) { return true; }
    }

    return false;
}

bool PowerLimiterClass::usesSmartBufferPoweredInverter() const
{
    for (auto const& upInv : _inverters) {
        if (upInv->isSmartBufferPowered()) { return true; }
    }

    return false;
}

bool PowerLimiterClass::isGovernedBatteryPoweredInverterProducing() const
{
    for (auto const& upInv : _inverters) {
        if (upInv->isBatteryPowered() && upInv->isProducing()) { return true; }
    }
    return false;
}
