// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "InverterAbstract.h"
#include <Arduino.h>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

class HM_Abstract : public InverterAbstract {
public:
    explicit HM_Abstract(HoymilesRadio* radio, const uint64_t serial);
    bool sendStatsRequest();
    bool sendAlarmLogRequest(const bool force = false);
    bool sendDevInfoRequest();
    bool sendSystemConfigParaRequest();
    bool sendActivePowerControlRequest(float limit, const PowerLimitControlType type);
    bool resendActivePowerControlRequest();
    bool sendPowerControlRequest(const bool turnOn);
    bool sendRestartControlRequest();
    bool resendPowerControlRequest();
    bool sendGridOnProFileParaRequest();
    bool sendFirmwareUpdateRequest(const String& littleFsPath,
                                   const uint8_t* rawAscii,
                                   const size_t rawAsciiLen) override;
    bool getFirmwareUpdateRunning() override;
    void abortFirmwareUpdateRequest() override;
    void resendFirmwareRow(const uint8_t* rowData, const uint16_t rowLen, const uint8_t attempt) override;
    void onFirmwareRowCompleted() override;
    bool supportsPowerDistributionLogic() override;

protected:
    float _activePowerControlLimit = 0;
    PowerLimitControlType _activePowerControlType = PowerLimitControlType::AbsolutNonPersistent;

private:
    void enqueueFirmwareRow(const uint8_t* rowData, const uint16_t rowLen, const bool jumpQueue, const uint8_t attempt = 1);
    // Pops the next parked row and enqueues its packets. Returns false if
    // there was nothing left to enqueue.
    bool enqueueNextFirmwareRow();

    uint8_t _lastAlarmLogCnt = 0;
    uint8_t _powerState = 1;

    String _fwFsPath;                          // non-empty: read from LittleFS
    const uint8_t* _fwPsramAscii = nullptr;    // non-null: raw ASCII source
    size_t _fwPsramAsciiLen = 0;
    uint32_t* _fwLineOffsets = nullptr;        // byte offset per Intel-Hex line
    uint16_t* _fwLineLengths = nullptr;        // ASCII length per line
    size_t _fwLineCount = 0;
    size_t _fwNextLineIndex = 0;
    std::mutex _pendingFirmwareRowsMutex;

    // Scans the source once and fills _fwLineOffsets/_fwLineLengths. Caller
    // must hold _pendingFirmwareRowsMutex.
    bool buildFirmwareLineIndex_unlocked();
    // Reads line `index` ASCII bytes (max maxLen) into `out`. Returns true
    // on success and sets outLen. Caller must hold the mutex.
    bool readFirmwareLineAscii_unlocked(size_t index, char* out, size_t maxLen, size_t& outLen);
    // Releases source + offset table. Caller must hold the mutex.
    void closeFirmwareSource_unlocked();
};
