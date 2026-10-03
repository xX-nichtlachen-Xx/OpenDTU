// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "InverterAbstract.h"
#include <Arduino.h>
#include <cstddef>
#include <cstdint>
#include <esp_partition.h>
#include <mutex>
#include <vector>

class HM_Abstract : public InverterAbstract {
public:
    explicit HM_Abstract(HoymilesRadio* radio, const uint64_t serial);
    bool sendStatsRequest();
    bool sendAlarmLogRequest(const bool force = false);
    bool sendDevInfoRequest(const bool force = false);
    bool sendSystemConfigParaRequest();
    bool sendActivePowerControlRequest(float limit, const PowerLimitControlType type);
    bool resendActivePowerControlRequest();
    bool sendPowerControlRequest(const bool turnOn);
    bool sendRestartControlRequest();
    bool resendPowerControlRequest();
    bool sendGridOnProFileParaRequest();
    bool sendFirmwareUpdateRequest(const uint8_t* rawAscii,
                                   const size_t rawAsciiLen,
                                   const esp_partition_t* otaPartition = nullptr,
                                   const size_t otaPartitionLen = 0) override;
    bool getFirmwareUpdateRunning() override;
    uint8_t getFirmwareUpdateProgress() const override;
    void abortFirmwareUpdateRequest() override;
    void failFirmwareUpdateRequest() override;
    void resendFirmwareRow(const uint8_t* rowData, const uint16_t rowLen, const uint8_t attempt) override;
    void onFirmwareRowCompleted() override;
    FirmwareUpdateResult getFirmwareUpdateResult() const override;
    bool supportsPowerDistributionLogic() override;

protected:
    float _activePowerControlLimit = 0;
    PowerLimitControlType _activePowerControlType = PowerLimitControlType::AbsolutNonPersistent;

private:
    void enqueueFirmwareRow(const uint8_t* rowData, const uint16_t rowLen, const bool jumpQueue, const uint8_t attempt = 1);
    bool enqueueNextFirmwareRow();

    uint8_t _lastAlarmLogCnt = 0;
    uint8_t _powerState = 1;

    const uint8_t* _fwPsramAscii = nullptr;
    size_t _fwPsramAsciiLen = 0;
    const esp_partition_t* _fwOtaPartition = nullptr;
    size_t _fwOtaLen = 0;
    uint32_t* _fwLineOffsets = nullptr;
    uint16_t* _fwLineLengths = nullptr;
    size_t _fwLineCount = 0;
    size_t _fwNextLineIndex = 0;
    uint8_t _firmwareUpdateProgress = 0;
    bool _firmwareUpdateAborted = false;
    FirmwareUpdateResult _firmwareUpdateResult = FirmwareUpdateResult::None;
    std::mutex _pendingFirmwareRowsMutex;

    bool buildFirmwareLineIndex_unlocked();
    bool readFirmwareLineAscii_unlocked(size_t index, char* out, size_t maxLen, size_t& outLen);
    void closeFirmwareSource_unlocked();
};
