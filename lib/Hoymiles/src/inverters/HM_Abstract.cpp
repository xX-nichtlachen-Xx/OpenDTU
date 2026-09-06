// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022-2026 Thomas Basler and others
 */
#include "HM_Abstract.h"
#include "HoymilesRadio.h"
#include "HoymilesRadio_NRF.h"
#include "commands/ActivePowerControlCommand.h"
#include "commands/AlarmDataCommand.h"
#include "commands/DevInfoAllCommand.h"
#include "commands/DevInfoSimpleCommand.h"
#include "commands/FirmwareDataCommand.h"
#include "commands/GridOnProFilePara.h"
#include "commands/PowerControlCommand.h"
#include "commands/RealTimeRunDataCommand.h"
#include "commands/SystemConfigParaCommand.h"
#include "utils/IntelHex.h"
#include <algorithm>
#include <cstring>
#include <esp_heap_caps.h>

HM_Abstract::HM_Abstract(HoymilesRadio* radio, const uint64_t serial)
    : InverterAbstract(radio, serial)
{
}

bool HM_Abstract::sendStatsRequest()
{
    if (!getEnablePolling()) {
        return false;
    }

    time_t now;
    time(&now);

    auto cmd = _radio->prepareCommand<RealTimeRunDataCommand>(this);
    cmd->setTime(now);
    _radio->enqueCommand(cmd);

    return true;
}

bool HM_Abstract::sendAlarmLogRequest(const bool force)
{
    if (!getEnablePolling()) {
        return false;
    }

    if (!force) {
        if (Statistics()->hasChannelFieldValue(TYPE_INV, CH0, FLD_EVT_LOG)) {
            if (static_cast<uint8_t>(Statistics()->getChannelFieldValue(TYPE_INV, CH0, FLD_EVT_LOG) == _lastAlarmLogCnt)) {
                return false;
            }
        }
    }

    _lastAlarmLogCnt = static_cast<uint8_t>(Statistics()->getChannelFieldValue(TYPE_INV, CH0, FLD_EVT_LOG));

    time_t now;
    time(&now);

    auto cmd = _radio->prepareCommand<AlarmDataCommand>(this);
    cmd->setTime(now);
    EventLog()->setLastAlarmRequestSuccess(CMD_PENDING);
    _radio->enqueCommand(cmd);

    return true;
}

bool HM_Abstract::sendDevInfoRequest()
{
    if (!getEnablePolling()) {
        return false;
    }

    time_t now;
    time(&now);

    auto cmdAll = _radio->prepareCommand<DevInfoAllCommand>(this);
    cmdAll->setTime(now);
    _radio->enqueCommand(cmdAll);

    auto cmdSimple = _radio->prepareCommand<DevInfoSimpleCommand>(this);
    cmdSimple->setTime(now);
    _radio->enqueCommand(cmdSimple);

    return true;
}

bool HM_Abstract::sendSystemConfigParaRequest()
{
    if (!getEnablePolling()) {
        return false;
    }

    time_t now;
    time(&now);

    auto cmd = _radio->prepareCommand<SystemConfigParaCommand>(this);
    cmd->setTime(now);
    SystemConfigPara()->setLastLimitRequestSuccess(CMD_PENDING);
    _radio->enqueCommand(cmd);

    return true;
}

bool HM_Abstract::sendActivePowerControlRequest(float limit, const PowerLimitControlType type)
{
    if (!getEnableCommands()) {
        return false;
    }

    if (type == PowerLimitControlType::RelativNonPersistent || type == PowerLimitControlType::RelativPersistent) {
        limit = min<float>(100, limit);
    }

    _activePowerControlLimit = limit;
    _activePowerControlType = type;

    auto cmd = _radio->prepareCommand<ActivePowerControlCommand>(this);
    cmd->setActivePowerLimit(limit, type);
    SystemConfigPara()->setLastLimitCommandSuccess(CMD_PENDING);
    _radio->enqueCommand(cmd);

    return true;
}

bool HM_Abstract::resendActivePowerControlRequest()
{
    return sendActivePowerControlRequest(_activePowerControlLimit, _activePowerControlType);
}

bool HM_Abstract::sendPowerControlRequest(const bool turnOn)
{
    if (!getEnableCommands()) {
        return false;
    }

    if (turnOn) {
        _powerState = 1;
    } else {
        _powerState = 0;
    }

    auto cmd = _radio->prepareCommand<PowerControlCommand>(this);
    cmd->setPowerOn(turnOn);
    PowerCommand()->setLastPowerCommandSuccess(CMD_PENDING);
    _radio->enqueCommand(cmd);

    return true;
}

bool HM_Abstract::sendRestartControlRequest()
{
    if (!getEnableCommands()) {
        return false;
    }

    _powerState = 2;

    auto cmd = _radio->prepareCommand<PowerControlCommand>(this);
    cmd->setRestart();
    PowerCommand()->setLastPowerCommandSuccess(CMD_PENDING);
    _radio->enqueCommand(cmd);

    return true;
}

bool HM_Abstract::resendPowerControlRequest()
{
    switch (_powerState) {
    case 0:
        return sendPowerControlRequest(false);
        break;
    case 1:
        return sendPowerControlRequest(true);
        break;
    case 2:
        return sendRestartControlRequest();
        break;

    default:
        return false;
        break;
    }
}

bool HM_Abstract::sendGridOnProFileParaRequest()
{
    if (!getEnablePolling()) {
        return false;
    }

    time_t now;
    time(&now);

    auto cmd = _radio->prepareCommand<GridOnProFilePara>(this);
    cmd->setTime(now);
    _radio->enqueCommand(cmd);

    return true;
}

bool HM_Abstract::sendFirmwareUpdateRequest(const uint8_t* rawAscii,
                                            const size_t rawAsciiLen,
                                            const esp_partition_t* otaPartition,
                                            const size_t otaPartitionLen)
{
    if (!getEnableCommands()) {
        return false;
    }

    const bool hasPsramSource = (rawAscii != nullptr && rawAsciiLen > 0);
    const bool hasOtaSource = (otaPartition != nullptr && otaPartitionLen > 0);
    if (hasPsramSource == hasOtaSource) {
        return false; // must have exactly one source
    }

    _firmwareUpdateAborted = false;
    _firmwareUpdateResult = FirmwareUpdateResult::None;

    // Firmware data is transmitted per Intel-Hex "row" (one row per line of
    // the uploaded .hex file, each row being that line's fully hex-decoded
    // bytes MINUS its own trailing line checksum). Each row is chunked into
    // packets of at most 16 bytes; the packet number ("nub") RESETS to 1 at
    // the start of every row, and only the LAST packet of a row gets the
    // 0x80 bit set plus a trailing 2-byte CRC16 (Modbus, start 0xFFFF, big-
    // endian) computed over the WHOLE row.
    {
        std::lock_guard<std::mutex> lock(_pendingFirmwareRowsMutex);
        closeFirmwareSource_unlocked();

        _fwPsramAscii = hasPsramSource ? rawAscii : nullptr;
        _fwPsramAsciiLen = hasPsramSource ? rawAsciiLen : 0;
        _fwOtaPartition = hasOtaSource ? otaPartition : nullptr;
        _fwOtaLen = hasOtaSource ? otaPartitionLen : 0;

        if (!buildFirmwareLineIndex_unlocked() || _fwLineCount == 0) {
            closeFirmwareSource_unlocked();
            return false;
        }
        _fwNextLineIndex = 0;
    }

    return enqueueNextFirmwareRow();
}

bool HM_Abstract::enqueueNextFirmwareRow()
{
    if (_firmwareUpdateAborted) {
        return false;
    }

    // MAX Intel-Hex line ASCII length -- byte count field is uint8_t, so
    // worst case per line is ~1 (':') + 8 (header) + 2*255 (data) + 2 (chk)
    // = 521 chars. Round up to 544 for slack.
    char lineAscii[544];
    uint8_t rowBytes[272];
    size_t rowLen = 0;

    {
        std::lock_guard<std::mutex> lock(_pendingFirmwareRowsMutex);
        while (_fwNextLineIndex < _fwLineCount) {
            const size_t index = _fwNextLineIndex++;

            size_t asciiLen = 0;
            if (!readFirmwareLineAscii_unlocked(index, lineAscii, sizeof(lineAscii), asciiLen)) {
                continue;
            }

            size_t decodedLen = 0;
            const IntelHex::RowResult result = IntelHex::decodeRow(lineAscii, asciiLen, rowBytes, decodedLen);
            if (result == IntelHex::RowResult::Skip || decodedLen == 0) {
                continue;
            }
            if (result == IntelHex::RowResult::Error) {
                closeFirmwareSource_unlocked();
                _firmwareUpdateResult = FirmwareUpdateResult::Failed;
                return false;
            }

            rowLen = decodedLen;
            if (result == IntelHex::RowResult::Eof) {
                // Row is transmitted (Intel-Hex EOF row is meaningful data),
                // and no more lines are read after it.
                _fwNextLineIndex = _fwLineCount;
            }
            break;
        }
    }

    if (rowLen == 0) {
        return false;
    }

    // Chunker copies out of `rowBytes` inline before the stack frame goes
    // away (each FirmwareDataCommand::setPayload() memcpys its slice).
    enqueueFirmwareRow(rowBytes, static_cast<uint16_t>(rowLen), false);
    return true;
}

void HM_Abstract::onFirmwareRowCompleted()
{
    if (_firmwareUpdateAborted) {
        return;
    }
    if (!enqueueNextFirmwareRow()) {
        // No parked row got enqueued -- either all rows are done (success) or
        // enqueueNextFirmwareRow() already recorded a decode failure above.
        std::lock_guard<std::mutex> lock(_pendingFirmwareRowsMutex);
        if (_firmwareUpdateResult == FirmwareUpdateResult::None) {
            _firmwareUpdateResult = FirmwareUpdateResult::Success;
            closeFirmwareSource_unlocked();
        }
    }
}

void HM_Abstract::resendFirmwareRow(const uint8_t* rowData, const uint16_t rowLen, const uint8_t attempt)
{
    if (_firmwareUpdateAborted) {
        return;
    }
    enqueueFirmwareRow(rowData, rowLen, true, attempt);
}

void HM_Abstract::enqueueFirmwareRow(const uint8_t* rowData, const uint16_t rowLen, const bool jumpQueue, const uint8_t attempt)
{
    constexpr uint8_t firmwareChunkSize = 16;
    constexpr uint8_t crcSize = 2;
    std::vector<std::shared_ptr<CommandAbstract>> packets;
    uint8_t packetNo = 1;

    for (size_t rowOffset = 0; rowOffset < rowLen; rowOffset += firmwareChunkSize) {
        const uint8_t chunkLen = static_cast<uint8_t>(std::min<size_t>(firmwareChunkSize, rowLen - rowOffset));
        const bool isLastDataChunk = (rowOffset + chunkLen >= rowLen);
        const bool crcFitsHere = isLastDataChunk && chunkLen <= firmwareChunkSize - crcSize;
        const bool useFinalRowMarker = isLastDataChunk && crcFitsHere;
        const uint8_t packetId = useFinalRowMarker
            ? (0x80 + packetNo)
            : (0x0 + packetNo);

        auto dataCmd = _radio->prepareCommand<FirmwareDataCommand>(this);
        dataCmd->setPacketNumber(packetId);
        dataCmd->setPayload(rowData + rowOffset, chunkLen);
        //dataCmd->setRowData(rowData, static_cast<uint8_t>(rowLen));
        dataCmd->setRowAttempt(attempt);
        if (crcFitsHere) {
            dataCmd->appendRowCrc(rowData, static_cast<uint8_t>(rowLen));
        }

        if (jumpQueue) {
            packets.push_back(dataCmd);
        } else {
            _radio->enqueCommand(dataCmd);
        }

        packetNo++;

        if (isLastDataChunk && !crcFitsHere) {
            auto crcCmd = _radio->prepareCommand<FirmwareDataCommand>(this);
            crcCmd->setPacketNumber(static_cast<uint8_t>(0x80 + packetNo));
            crcCmd->setPayload(rowData, 0);
            crcCmd->appendRowCrc(rowData, static_cast<uint8_t>(rowLen));
            crcCmd->setRowAttempt(attempt);

            if (jumpQueue) {
                packets.push_back(crcCmd);
            } else {
                _radio->enqueCommand(crcCmd);
            }

            packetNo++;
        }
    }

    // Re-enqueue the retransmitted row packets in reverse so the queue order
    // matches the original row packet order.
    for (auto it = packets.rbegin(); it != packets.rend(); ++it) {
        _radio->enqueCommand(*it);
    }
}

bool HM_Abstract::getFirmwareUpdateRunning()
{
    if (_firmwareUpdateAborted) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(_pendingFirmwareRowsMutex);
        if (_fwLineCount > 0 && _fwNextLineIndex < _fwLineCount) {
            return true;
        }
    }
    return _radio->hasFirmwareUpdateCommands(this);
}

void HM_Abstract::abortFirmwareUpdateRequest()
{
    _firmwareUpdateAborted = true;

    // Drop parked rows first so a row-ack that lands right now cannot
    // re-enqueue a fresh packet after we cleared the radio queue.
    {
        std::lock_guard<std::mutex> lock(_pendingFirmwareRowsMutex);
        _firmwareUpdateResult = FirmwareUpdateResult::Aborted;
        closeFirmwareSource_unlocked();
    }
}

void HM_Abstract::failFirmwareUpdateRequest()
{
    _firmwareUpdateAborted = true;

    {
        std::lock_guard<std::mutex> lock(_pendingFirmwareRowsMutex);
        _firmwareUpdateResult = FirmwareUpdateResult::Failed;
        closeFirmwareSource_unlocked();
    }
}

FirmwareUpdateResult HM_Abstract::getFirmwareUpdateResult() const
{
    return _firmwareUpdateResult;
}

bool HM_Abstract::buildFirmwareLineIndex_unlocked()
{
    // First pass: count lines (LF-terminated). Second pass: fill offsets.
    // We split into two passes so the offset table can be allocated exactly
    // in one shot in PSRAM.
    size_t count = 0;
    auto forEachLine = [&](auto&& emit) -> bool {
        if (_fwOtaPartition != nullptr && _fwOtaLen > 0) {
            // Streams straight from flash in chunks instead of requiring the
            // whole (potentially large) image to be pointer-addressable in
            // RAM -- this is the no-PSRAM fallback source, so avoiding a full
            // in-memory copy is the entire point.
            constexpr size_t chunkSize = 512;
            uint8_t chunk[chunkSize];
            uint32_t start = 0;
            uint32_t pos = 0;
            while (pos < _fwOtaLen) {
                const size_t toRead = std::min(chunkSize, static_cast<size_t>(_fwOtaLen - pos));
                if (esp_partition_read(_fwOtaPartition, pos, chunk, toRead) != ESP_OK) {
                    return false;
                }
                for (size_t i = 0; i < toRead; ++i) {
                    ++pos;
                    if (chunk[i] == '\n') {
                        if (pos - 1 > start) {
                            emit(start, static_cast<uint16_t>(pos - 1 - start));
                        }
                        start = pos;
                    }
                }
            }
            if (pos > start) {
                emit(start, static_cast<uint16_t>(pos - start));
            }
            return true;
        }

        if (_fwPsramAscii == nullptr || _fwPsramAsciiLen == 0) {
            return false;
        }
        size_t start = 0;
        for (size_t i = 0; i < _fwPsramAsciiLen; ++i) {
            if (_fwPsramAscii[i] == '\n') {
                if (i > start) {
                    emit(static_cast<uint32_t>(start), static_cast<uint16_t>(i - start));
                }
                start = i + 1;
            }
        }
        if (start < _fwPsramAsciiLen) {
            emit(static_cast<uint32_t>(start), static_cast<uint16_t>(_fwPsramAsciiLen - start));
        }
        return true;
    };

    if (!forEachLine([&](uint32_t, uint16_t) { ++count; })) {
        return false;
    }
    if (count == 0) {
        return false;
    }

    const auto indexAlloc = [](size_t bytes) -> void* {
        // Line-index stays in internal DRAM: PSRAM byte-access is a lot
        // slower than DRAM, and 18 KB for a ~3000-line firmware easily fits.
        void* p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
        if (p == nullptr) {
            p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
        }
        return p;
    };

    _fwLineOffsets = static_cast<uint32_t*>(indexAlloc(count * sizeof(uint32_t)));
    _fwLineLengths = static_cast<uint16_t*>(indexAlloc(count * sizeof(uint16_t)));
    if (_fwLineOffsets == nullptr || _fwLineLengths == nullptr) {
        if (_fwLineOffsets) { heap_caps_free(_fwLineOffsets); _fwLineOffsets = nullptr; }
        if (_fwLineLengths) { heap_caps_free(_fwLineLengths); _fwLineLengths = nullptr; }
        return false;
    }
    _fwLineCount = count;

    size_t i = 0;
    forEachLine([&](uint32_t offset, uint16_t len) {
        if (i < count) {
            _fwLineOffsets[i] = offset;
            _fwLineLengths[i] = len;
            ++i;
        }
    });

    return i == count;
}

bool HM_Abstract::readFirmwareLineAscii_unlocked(size_t index, char* out, size_t maxLen, size_t& outLen)
{
    outLen = 0;
    if (index >= _fwLineCount || _fwLineOffsets == nullptr || _fwLineLengths == nullptr) {
        return false;
    }
    const uint16_t len = _fwLineLengths[index];
    if (len == 0 || len > maxLen) {
        return false;
    }

    if (_fwOtaPartition != nullptr) {
        const uint32_t off = _fwLineOffsets[index];
        if (static_cast<size_t>(off) + len > _fwOtaLen) {
            return false;
        }
        if (esp_partition_read(_fwOtaPartition, off, out, len) != ESP_OK) {
            return false;
        }
    } else if (_fwPsramAscii != nullptr) {
        const size_t off = _fwLineOffsets[index];
        if (off + len > _fwPsramAsciiLen) {
            return false;
        }
        memcpy(out, _fwPsramAscii + off, len);
    } else {
        return false;
    }

    outLen = len;
    return true;
}

void HM_Abstract::closeFirmwareSource_unlocked()
{
    if (_fwLineOffsets != nullptr) {
        heap_caps_free(_fwLineOffsets);
        _fwLineOffsets = nullptr;
    }
    if (_fwLineLengths != nullptr) {
        heap_caps_free(_fwLineLengths);
        _fwLineLengths = nullptr;
    }
    _fwLineCount = 0;
    _fwNextLineIndex = 0;
    _fwPsramAscii = nullptr;
    _fwPsramAsciiLen = 0;
    _fwOtaPartition = nullptr;
    _fwOtaLen = 0;
}

bool HM_Abstract::supportsPowerDistributionLogic()
{
    return false;
}
