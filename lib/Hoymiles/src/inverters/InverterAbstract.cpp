// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022-2026 Thomas Basler and others
 */
#include "InverterAbstract.h"
#include "../HoymilesCrypto.h"
#include "crc.h"
#include <cstring>
#include <esp_log.h>
#include <esp_random.h>

#undef TAG
static const char* TAG = "hoymiles";

InverterAbstract::InverterAbstract(HoymilesRadio* radio, const uint64_t serial)
{
    _serial.u64 = serial;
    _radio = radio;

    char serial_buff[sizeof(uint64_t) * 8 + 1];
    snprintf(serial_buff, sizeof(serial_buff), "%0" PRIx32 "%08" PRIx32,
        static_cast<uint32_t>((serial >> 32) & 0xFFFFFFFF),
        static_cast<uint32_t>(serial & 0xFFFFFFFF));
    _serialString = serial_buff;

    _alarmLogParser.reset(new AlarmLogParser());
    _devInfoParser.reset(new DevInfoParser());
    _gridProfileParser.reset(new GridProfileParser());
    _powerCommandParser.reset(new PowerCommandParser());
    _statisticsParser.reset(new StatisticsParser());
    _systemConfigParaParser.reset(new SystemConfigParaParser());
}

void InverterAbstract::init()
{
    // This has to be done here because:
    // Not possible in constructor --> virtual function
    // Not possible in verifyAllFragments --> Because no data if nothing is ever received
    // It has to be executed because otherwise the getChannelCount method in stats always returns 0
    _statisticsParser.get()->setByteAssignment(getByteAssignment(), getByteAssignmentSize());
}

uint64_t InverterAbstract::serial() const
{
    return _serial.u64;
}

const String& InverterAbstract::serialString() const
{
    return _serialString;
}

void InverterAbstract::setName(const char* name)
{
    uint8_t len = strlen(name);
    if (len + 1 > MAX_NAME_LENGTH) {
        len = MAX_NAME_LENGTH - 1;
    }
    strncpy(_name, name, len);
    _name[len] = '\0';
}

const char* InverterAbstract::name() const
{
    return _name;
}

bool InverterAbstract::isProducing()
{
    float totalAc = 0;
    for (auto& c : Statistics()->getChannelsByType(TYPE_AC)) {
        if (Statistics()->hasChannelFieldValue(TYPE_AC, c, FLD_PAC)) {
            totalAc += Statistics()->getChannelFieldValue(TYPE_AC, c, FLD_PAC);
        }
    }

    return _enablePolling && totalAc > 0;
}

bool InverterAbstract::isReachable()
{
    return _enablePolling && Statistics()->getRxFailureCount() <= _reachableThreshold;
}

void InverterAbstract::setEnablePolling(const bool enabled)
{
    _enablePolling = enabled;
}

bool InverterAbstract::getEnablePolling() const
{
    return _enablePolling;
}

void InverterAbstract::setEnableCommands(const bool enabled)
{
    _enableCommands = enabled;
}

bool InverterAbstract::getEnableCommands() const
{
    return _enableCommands;
}

void InverterAbstract::setReachableThreshold(const uint8_t threshold)
{
    _reachableThreshold = threshold;
}

uint8_t InverterAbstract::getReachableThreshold() const
{
    return _reachableThreshold;
}

void InverterAbstract::setZeroValuesIfUnreachable(const bool enabled)
{
    _zeroValuesIfUnreachable = enabled;
}

bool InverterAbstract::getZeroValuesIfUnreachable() const
{
    return _zeroValuesIfUnreachable;
}

void InverterAbstract::setZeroYieldDayOnMidnight(const bool enabled)
{
    _zeroYieldDayOnMidnight = enabled;
}

bool InverterAbstract::getZeroYieldDayOnMidnight() const
{
    return _zeroYieldDayOnMidnight;
}

void InverterAbstract::setClearEventlogOnMidnight(const bool enabled)
{
    _clearEventlogOnMidnight = enabled;
}

bool InverterAbstract::getClearEventlogOnMidnight() const
{
    return _clearEventlogOnMidnight;
}

int8_t InverterAbstract::getLastRssi() const
{
    return _lastRssi;
}

bool InverterAbstract::sendChangeChannelRequest()
{
    return false;
}

HoymilesRadio* InverterAbstract::getRadio()
{
    return _radio;
}

AlarmLogParser* InverterAbstract::EventLog()
{
    return _alarmLogParser.get();
}

DevInfoParser* InverterAbstract::DevInfo()
{
    return _devInfoParser.get();
}

GridProfileParser* InverterAbstract::GridProfile()
{
    return _gridProfileParser.get();
}

PowerCommandParser* InverterAbstract::PowerCommand()
{
    return _powerCommandParser.get();
}

StatisticsParser* InverterAbstract::Statistics()
{
    return _statisticsParser.get();
}

SystemConfigParaParser* InverterAbstract::SystemConfigPara()
{
    return _systemConfigParaParser.get();
}

void InverterAbstract::clearRxFragmentBuffer()
{
    memset(_rxFragmentBuffer, 0, MAX_RF_FRAGMENT_COUNT * sizeof(fragment_t));
    _rxFragmentMaxPacketId = 0;
    _rxFragmentLastPacketId = 0;
    _rxFragmentRetransmitCnt = 0;
}

void InverterAbstract::addRxFragment(const uint8_t fragment[], const uint8_t len, const int8_t rssi)
{
    _lastRssi = rssi;

    if (len < 11) {
        ESP_LOGE(TAG, "(%s, %d) fragment too short", __FILE__, __LINE__);
        return;
    }

    if (len - 11 > MAX_RF_PAYLOAD_SIZE) {
        ESP_LOGE(TAG, "FATAL: (%s, %d) fragment too large", __FILE__, __LINE__);
        return;
    }

    const uint8_t fragmentCount = fragment[9];

    // Packets with 0x81 will be seen as 1
    const uint8_t fragmentId = fragmentCount & 0b01111111; // fragmentId is 1 based

    if (fragmentId == 0) {
        ESP_LOGE(TAG, "Fragment id zero received and ignored");
        return;
    }

    if (fragmentId >= MAX_RF_FRAGMENT_COUNT) {
        ESP_LOGE(TAG, "Fragment id %" PRIu8 " is too large for buffer and ignored", fragmentId);
        return;
    }

    memcpy(_rxFragmentBuffer[fragmentId - 1].fragment, &fragment[10], len - 11);
    _rxFragmentBuffer[fragmentId - 1].len = len - 11;
    _rxFragmentBuffer[fragmentId - 1].mainCmd = fragment[0];
    _rxFragmentBuffer[fragmentId - 1].wasReceived = true;

    if (fragmentId > _rxFragmentLastPacketId) {
        _rxFragmentLastPacketId = fragmentId;
    }

    // 0b10000000 == 0x80
    if ((fragmentCount & 0b10000000) == 0b10000000) {
        _rxFragmentMaxPacketId = fragmentId;
    }
}

// Returns Zero on Success or the Fragment ID for retransmit or error code
uint8_t InverterAbstract::verifyAllFragments(CommandAbstract& cmd)
{
    // All missing
    if (_rxFragmentLastPacketId == 0) {
        ESP_LOGW(TAG, "All missing");
        if (cmd.getSendCount() <= cmd.getMaxResendCount()) {
            return FRAGMENT_ALL_MISSING_RESEND;
        } else {
            cmd.gotTimeout();
            return FRAGMENT_ALL_MISSING_TIMEOUT;
        }
    }

    // Last fragment is missing (the one with 0x80)
    if (_rxFragmentMaxPacketId == 0) {
        ESP_LOGW(TAG, "Last missing");
        if (_rxFragmentRetransmitCnt++ < cmd.getMaxRetransmitCount()) {
            return _rxFragmentLastPacketId + 1;
        } else {
            cmd.gotTimeout();
            return FRAGMENT_RETRANSMIT_TIMEOUT;
        }
    }

    // Middle fragment is missing
    for (uint8_t i = 0; i < _rxFragmentMaxPacketId - 1; i++) {
        if (!_rxFragmentBuffer[i].wasReceived) {
            ESP_LOGW(TAG, "Middle missing");
            if (_rxFragmentRetransmitCnt++ < cmd.getMaxRetransmitCount()) {
                return i + 1;
            } else {
                cmd.gotTimeout();
                return FRAGMENT_RETRANSMIT_TIMEOUT;
            }
        }
    }

    bool handled = cmd.handleResponse(_rxFragmentBuffer, _rxFragmentMaxPacketId);
    if (!handled && isEncryptionActive()) {
        decryptResponseFragments();
        handled = cmd.handleResponse(_rxFragmentBuffer, _rxFragmentMaxPacketId);
    }

    if (!handled) {
        cmd.gotTimeout();
        return FRAGMENT_HANDLE_ERROR;
    }

    return FRAGMENT_OK;
}

void InverterAbstract::performDailyTask()
{
    // Have to reset the offets first, otherwise it will
    // Substract the offset from zero which leads to a high value
    Statistics()->resetYieldDayCorrection();
    if (getZeroYieldDayOnMidnight()) {
        Statistics()->zeroDailyData();
    }
    if (getClearEventlogOnMidnight()) {
        EventLog()->clearBuffer();
    }
    resetRadioStats();
}

void InverterAbstract::resetRadioStats()
{
    RadioStats = {};
}

void InverterAbstract::setEncryptionEnabled(const bool enabled)
{
    _encryptionEnabled = enabled;
    if (!enabled) {
        _encryptionSessionActive = false;
        _encryptionKeyValid = false;
        _encryptionConfigValid = false;
        _encryptionSessionConfirmed = false;
        _encryptionFailReported = false;
    }
}

bool InverterAbstract::getEncryptionEnabled() const
{
    return _encryptionEnabled;
}

bool InverterAbstract::isEncryptionActive() const
{
    return _encryptionEnabled && _encryptionSessionActive;
}

void InverterAbstract::setEncryptionSessionActive(const bool active)
{
    if (active && !_encryptionSessionActive) {
        ESP_LOGD(TAG, "Encryption session active, pending confirmation (inv %s)", _serialString.c_str());
    }
    if (!active) {
        _encryptionSessionConfirmed = false;
    }
    _encryptionSessionActive = active;
}

void InverterAbstract::setEncryptionSeed(const uint8_t seed[16])
{
    memcpy(_encryptionSeed, seed, 16);
    _encryptionKeyValid = false; // force re-derivation
    deriveConfigKeys();
}

const uint8_t* InverterAbstract::getEncryptionSeed() const
{
    return _encryptionSeed;
}

void InverterAbstract::generateEncryptionSeed()
{
    for (uint8_t i = 0; i < 14; i++) {
        _encryptionSeed[i] = static_cast<uint8_t>(esp_random());
    }
    const uint16_t chk = crc16(_encryptionSeed, 14);
    _encryptionSeed[14] = static_cast<uint8_t>(chk >> 8);
    _encryptionSeed[15] = static_cast<uint8_t>(chk);
    _encryptionKeyValid = false;
    _encryptionFailReported = false;
    deriveConfigKeys();
    ESP_LOGD(TAG, "Encryption handshake started (inv %s, seed fp %02x)",
        _serialString.c_str(), crc8(_encryptionSeed, 16));
}

void InverterAbstract::deriveConfigKeys()
{
    const uint8_t sn4[4] = {
        _serial.b[3], _serial.b[2], _serial.b[1], _serial.b[0]
    };
    HoymilesCrypto::deriveConfigKeyIv(_encryptionSeed, sn4, _encryptionConfigKey, _encryptionConfigIv);
    _encryptionConfigValid = true;
}

bool InverterAbstract::isConfigChannelValid() const
{
    return _encryptionEnabled && _encryptionConfigValid;
}

bool InverterAbstract::isEncryptionSessionConfirmed() const
{
    return _encryptionSessionConfirmed;
}

void InverterAbstract::encryptConfigBlock(uint8_t block[16])
{
    HoymilesCrypto::encryptBlock(_encryptionConfigKey, _encryptionConfigIv, block, block, 16);
}

#define ENCRYPTION_MAX_FAILURES 3

void InverterAbstract::handleEncryptionResult(const bool success)
{
    if (!isEncryptionActive()) {
        return;
    }
    if (success) {
        _encryptionFailCount = 0;
        if (!_encryptionSessionConfirmed) {
            // First real encrypted reply decoded -> the handshake actually worked.
            _encryptionSessionConfirmed = true;
            _encryptionFailReported = false;
            ESP_LOGI(TAG, "Encryption enabled for inverter %s", _serialString.c_str());
            ESP_LOGD(TAG, "Encryption session confirmed (inv %s)", _serialString.c_str());
        }
        return;
    }
    if (++_encryptionFailCount >= ENCRYPTION_MAX_FAILURES) {
        if (!_encryptionFailReported) {
            _encryptionFailReported = true;
            ESP_LOGW(TAG, "Encryption handshake failed for inverter %s", _serialString.c_str());
            ESP_LOGD(TAG, "Encryption handshake failed: %s (inv %s)",
                _encryptionSessionConfirmed ? "repeated rejects" : "inverter did not accept the session",
                _serialString.c_str());
        }
        ESP_LOGD(TAG, "Encryption session lost, re-handshaking (inv %s)", _serialString.c_str());
        // Drop the session; the next enqueueSeedIfNeeded() generates a fresh seed
        // and re-runs the 0x35/0x36 handshake.
        _encryptionSessionActive = false;
        _encryptionSessionConfirmed = false;
        _encryptionKeyValid = false;
        _encryptionConfigValid = false;
        _encryptionFailCount = 0;
    }
}

void InverterAbstract::updateSessionKeys(const uint32_t unixTime)
{
    if (_encryptionKeyValid && (_encryptionKeyTime / 300 == unixTime / 300)) {
        return;
    }

    const bool wasValid = _encryptionKeyValid;
    const uint32_t oldBucket = _encryptionKeyTime / 300;
    const uint32_t newBucket = unixTime / 300;

    const uint8_t sn4[4] = {
        _serial.b[3], _serial.b[2], _serial.b[1], _serial.b[0]
    };
    HoymilesCrypto::deriveKeyIv(_encryptionSeed, sn4, unixTime, _encryptionKey, _encryptionIv);
    _encryptionKeyTime = unixTime;
    _encryptionKeyValid = true;

    if (wasValid) {
        ESP_LOGD(TAG, "Rekey (bucket ..%02x -> ..%02x) (inv %s)",
            static_cast<unsigned>(oldBucket & 0xff), static_cast<unsigned>(newBucket & 0xff), _serialString.c_str());
    } else {
        ESP_LOGD(TAG, "Session key derived (fp %02x, bucket ..%02x) (inv %s)",
            crc8(_encryptionKey, 16), static_cast<unsigned>(newBucket & 0xff), _serialString.c_str());
    }
}

void InverterAbstract::encryptPayloadBlock(uint8_t block[16], const uint32_t unixTime)
{
    updateSessionKeys(unixTime);
    HoymilesCrypto::encryptBlock(_encryptionKey, _encryptionIv, block, block, 16);
}

void InverterAbstract::decryptPayloadBlock(uint8_t block[16])
{
    if (!_encryptionKeyValid) {
        return;
    }
    HoymilesCrypto::decryptBlock(_encryptionKey, _encryptionIv, block, block, 16);
}

void InverterAbstract::decryptResponseFragments()
{
    for (uint8_t i = 0; i < _rxFragmentMaxPacketId; i++) {
        if (_rxFragmentBuffer[i].len == 16) {
            decryptPayloadBlock(_rxFragmentBuffer[i].fragment);
        }
    }

    fragment_t& last = _rxFragmentBuffer[_rxFragmentMaxPacketId - 1];
    if (last.len == 16) {
        const uint8_t pad = last.fragment[15];
        if (pad >= 1 && pad <= 16) {
            bool validPad = true;
            for (uint8_t k = 0; k < pad; k++) {
                if (last.fragment[16 - 1 - k] != pad) {
                    validPad = false;
                    break;
                }
            }
            if (validPad) {
                last.len = 16 - pad;
            }
        }
    }
}
