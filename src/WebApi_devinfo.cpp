// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022-2026 Thomas Basler and others
 */
#include "WebApi_devinfo.h"
#include "WebApi.h"
#include "WebApi_errors.h"
#include "WebApi_file.h"
#include <AsyncJson.h>
#include <Hoymiles.h>
#include <ctime>
#include <esp_partition.h>
#include <vector>
#include "utils/IntelHex.h"

namespace {

bool isAsciiDigit(const char c)
{
    return c >= '0' && c <= '9';
}

struct FirmwareSerialRule {
    uint16_t preSerial;
    uint8_t newGen1;   // row byte [4] low nibble; high nibble ignored
    uint8_t phase;     // row byte [5] high nibble
    uint8_t inputType; // row byte [5] low nibble
    uint8_t dsp;       // row byte [6] high nibble
    uint8_t newGen2;   // row byte [6] low nibble
    uint8_t newGen3;   // row byte [7] high nibble
    uint8_t bType;     // row byte [7] low nibble
};

constexpr FirmwareSerialRule kFirmwareSerialRules[] = {
    // preSerial, newGen1, phase, inputType, dsp, newGen2, newGen3, bType
    { 0x1121, 0, 1, 0, 0, 0, 0, 0 }, // HM 1T MI
    { 0x1124, 0, 1, 0, 0, 0, 0, 0 }, // HMS 1T MI
    { 0x1125, 0, 1, 0, 0, 0, 0, 1 }, // HMS 1T B
    { 0x1126, 0, 1, 0, 0, 0, 0, 2 }, // HMS 1T US
    { 0x1400, 0, 1, 0, 0, 0, 0, 1 }, // HMS 1T B
    { 0x1141, 0, 1, 1, 0, 0, 0, 0 }, // HM 2T MI
    { 0x1143, 0, 1, 1, 1, 0, 0, 1 }, // HMS 2T B
    { 0x1144, 0, 1, 1, 1, 0, 0, 1 }, // HMS 2T B
    { 0x1146, 0, 1, 1, 1, 0, 0, 2 }, // HMS 2T US
    { 0x1410, 0, 1, 1, 1, 0, 0, 1 }, // HMS 2T B
    { 0x1161, 0, 1, 2, 0, 0, 0, 0 }, // HM 4T MI
    { 0x1162, 0, 4, 2, 0, 0, 0, 0 }, // HME1 4T MI
    { 0x1164, 0, 1, 2, 1, 0, 0, 0 }, // HMS 4T MI
    { 0x1165, 0, 1, 2, 2, 0, 0, 0 }, // HMS 4T MI (2000B_T)
    { 0x1166, 0, 1, 2, 1, 1, 0, 1 }, // HMS 4T B (2000C_B)
    { 0x1421, 0, 1, 2, 1, 1, 0, 1 }, // HMS 4T B (2000C_B)
    { 0x1620, 0, 1, 2, 3, 0, 0, 1 }, // HMS 4T B (WB_B)
    { 0x1361, 0, 3, 5, 0, 0, 0, 0 }, // HMT 4T MI
    { 0x1362, 0, 3, 6, 0, 0, 0, 0 }, // HMT 4T MI (NA R)
    { 0x1382, 0, 3, 3, 0, 0, 0, 0 }, // HMT 6T MI
    { 0x1520, 0, 1, 6, 0, 0, 0, 0 }, // MIT-5000 MI
};

static bool identityRowMatchesRule(const uint8_t rowBytes[8], const FirmwareSerialRule& rule)
{
    if ((rowBytes[0] & 0x0F) != rule.newGen1) {
        return false;
    }

    if ((rowBytes[1] >> 4) != rule.phase || (rowBytes[1] & 0x0F) != rule.inputType) {
        return false;
    }

    if ((rowBytes[2] >> 4) != rule.dsp || (rowBytes[2] & 0x0F) != rule.newGen2) {
        return false;
    }

    if ((rowBytes[3] >> 4) != rule.newGen3 || (rowBytes[3] & 0x0F) != rule.bType) {
        return false;
    }

    return true;
}

const FirmwareSerialRule* lookupFirmwareSerialRule(const uint64_t serial)
{
    const uint16_t preSerial = static_cast<uint16_t>((serial >> 32) & 0xffff);
    for (const auto& rule : kFirmwareSerialRules) {
        if (rule.preSerial == preSerial) {
            return &rule;
        }
    }
    return nullptr;
}

constexpr uint16_t kAllowedFirmwareUpdateSerialPrefixes[] = {
    0x1121, 0x1141, 0x1161, 0x1162, 0x1124, 0x1126, 0x1400,
    0x1125, 0x1143, 0x1144, 0x1146, 0x1410, 0x1361, 0x1362,
    0x1164, 0x1165, 0x1166, 0x1421, 0x1620, 0x1382,
};

bool isSerialAllowedForFirmwareUpdate(const uint64_t serial)
{
    const uint16_t preSerial = static_cast<uint16_t>((serial >> 32) & 0xffff);
    for (const uint16_t allowed : kAllowedFirmwareUpdateSerialPrefixes) {
        if (allowed == preSerial) {
            return true;
        }
    }
    return false;
}

const char* firmwareUpdateResultToString(const FirmwareUpdateResult result)
{
    switch (result) {
    case FirmwareUpdateResult::Success:
        return "success";
    case FirmwareUpdateResult::Failed:
        return "failed";
    case FirmwareUpdateResult::Aborted:
        return "aborted";
    default:
        return "none";
    }
}

bool parseHwModelChannelInfo(const String& hwModelName, bool& outIsThreePhase, uint8_t& outChannelCount, uint8_t& outDsp)
{
    outIsThreePhase = hwModelName.startsWith("HMT-");
    const bool isHms = hwModelName.startsWith("HMS-");
    outChannelCount = 0;
    outDsp = 0;

    for (unsigned int i = 0; i < hwModelName.length(); ++i) {
        if (hwModelName[i] != 'T') {
            continue;
        }
        unsigned int digitsStart = i;
        while (digitsStart > 0 && isAsciiDigit(hwModelName[digitsStart - 1])) {
            --digitsStart;
        }
        if (digitsStart == i) {
            continue; // no digit immediately before this 'T'
        }
        outChannelCount = static_cast<uint8_t>(hwModelName.substring(digitsStart, i).toInt());
        break;
    }
    if (outChannelCount == 0) {
        return false;
    }

    outDsp = (isHms && outChannelCount != 1) ? 0x10 : 0x00;
    return true;
}

bool readFirstFirmwareLine(const uint8_t* rawAscii, const size_t rawAsciiLen,
                           const esp_partition_t* otaPartition, const size_t otaLen,
                           char* out, const size_t maxLen, size_t& outLen)
{
    outLen = 0;

    if (otaPartition != nullptr && otaLen > 0) {
        while (outLen < otaLen && outLen < maxLen) {
            uint8_t b = 0;
            if (esp_partition_read(otaPartition, outLen, &b, 1) != ESP_OK) {
                break;
            }
            if (b == '\n') {
                break;
            }
            out[outLen] = static_cast<char>(b);
            ++outLen;
        }
        return outLen > 0;
    }

    if (rawAscii == nullptr || rawAsciiLen == 0) {
        return false;
    }

    while (outLen < rawAsciiLen && outLen < maxLen && rawAscii[outLen] != '\n') {
        out[outLen] = static_cast<char>(rawAscii[outLen]);
        ++outLen;
    }
    return outLen > 0;
}

bool firmwareFileMatchesInverter(const std::shared_ptr<InverterAbstract>& inv,
                                 const uint8_t* rawAscii,
                                 const size_t rawAsciiLen,
                                 const esp_partition_t* otaPartition,
                                 const size_t otaLen,
                                 String& outReason)
{
    const String hwModelName = inv->typeName();
    if (!isSerialAllowedForFirmwareUpdate(inv->serial())) {
        outReason = "Firmware update is not supported for this inverter!";
        return false;
    }

    if (hwModelName.isEmpty()) {
        outReason = "Inverter hardware model is not known yet (no device info received)!";
        return false;
    }

    const FirmwareSerialRule* rule = lookupFirmwareSerialRule(inv->serial());
    if (rule == nullptr) {
        outReason = "Firmware update is not supported for this inverter!";
        return false;
    }

    char lineAscii[64];
    size_t lineLen = 0;
    if (!readFirstFirmwareLine(rawAscii, rawAsciiLen, otaPartition, otaLen, lineAscii, sizeof(lineAscii), lineLen)) {
        outReason = "Firmware file could not be read!";
        return false;
    }

    uint8_t rowBytes[32];
    size_t rowLen = 0;
    if (IntelHex::decodeRow(lineAscii, lineLen, rowBytes, rowLen) != IntelHex::RowResult::Data || rowLen < 8) {
        outReason = "Firmware file has an unrecognized identity row!";
        return false;
    }

    const uint8_t identityRow[8] = {
        rowBytes[4], rowBytes[5], rowBytes[6], rowBytes[7], 0, 0, 0, 0
    };
    if (!identityRowMatchesRule(identityRow, *rule)) {
        outReason = "Firmware file does not match the connected inverter model (" + hwModelName + ")! Expected identity nibble pattern 1/"
            + String(rule->newGen1, HEX) + "/" + String(rule->phase, HEX) + "/" + String(rule->inputType, HEX) + "/"
            + String(rule->dsp, HEX) + "/" + String(rule->newGen2, HEX) + "/" + String(rule->newGen3, HEX) + "/" + String(rule->bType, HEX)
            + ", got " + String((identityRow[0] >> 4) & 0x0F, HEX) + "/" + String(identityRow[0] & 0x0F, HEX) + "/"
            + String((identityRow[1] >> 4) & 0x0F, HEX) + "/" + String(identityRow[1] & 0x0F, HEX) + "/"
            + String((identityRow[2] >> 4) & 0x0F, HEX) + "/" + String(identityRow[2] & 0x0F, HEX) + "/"
            + String((identityRow[3] >> 4) & 0x0F, HEX) + "/" + String(identityRow[3] & 0x0F, HEX) + "!";
        return false;
    }

    return true;
}

bool isFirmwareUpdateSupported(const std::shared_ptr<InverterAbstract>& inv)
{
    if (inv == nullptr || !isSerialAllowedForFirmwareUpdate(inv->serial())) {
        return false;
    }
    bool isThreePhase = false;
    uint8_t channelCount = 0;
    uint8_t dsp = 0;
    return parseHwModelChannelInfo(inv->typeName(), isThreePhase, channelCount, dsp);
}

String getFirmwareVariant(const std::shared_ptr<InverterAbstract>& inv)
{
    if (!isFirmwareUpdateSupported(inv)) {
        return "unsupported";
    }
    bool isThreePhase = false;
    uint8_t channelCount = 0;
    uint8_t dsp = 0;
    parseHwModelChannelInfo(inv->typeName(), isThreePhase, channelCount, dsp);
    return String(static_cast<unsigned int>(channelCount)) + "in1";
}

bool pickFirmwareSource(const uint8_t*& outRawAscii, size_t& outRawAsciiLen,
                        const esp_partition_t*& outOtaPartition, size_t& outOtaLen)
{
    outRawAscii = nullptr;
    outRawAsciiLen = 0;
    outOtaPartition = nullptr;
    outOtaLen = 0;

    size_t psramLen = 0;
    const uint8_t* psramPtr = peekFirmwareUploadInPsram(psramLen);
    if (psramPtr != nullptr && psramLen > 0) {
        outRawAscii = psramPtr;
        outRawAsciiLen = psramLen;
        return true;
    }

    const esp_partition_t* otaPartition = nullptr;
    size_t otaLen = 0;
    if (getFirmwareUploadInInactiveOtaSlot(otaPartition, otaLen)) {
        outOtaPartition = otaPartition;
        outOtaLen = otaLen;
        return true;
    }

    return false;
}
} // namespace

void WebApiDevInfoClass::init(AsyncWebServer& server, Scheduler& scheduler)
{
    using std::placeholders::_1;

    server.on("/api/devinfo/status", HTTP_GET, static_cast<ArRequestHandlerFunction>(std::bind(&WebApiDevInfoClass::onDevInfoStatus, this, _1)));
    server.on(AsyncURIMatcher::exact("/api/devinfo/update"), HTTP_POST, static_cast<ArRequestHandlerFunction>(std::bind(&WebApiDevInfoClass::onFirmwareUpdateStart, this, _1)));
    server.on("/api/devinfo/update/abort", HTTP_POST, static_cast<ArRequestHandlerFunction>(std::bind(&WebApiDevInfoClass::onFirmwareUpdateAbort, this, _1)));
    server.on("/api/devinfo/refresh", HTTP_POST, static_cast<ArRequestHandlerFunction>(std::bind(&WebApiDevInfoClass::onDevInfoRefresh, this, _1)));
}

void WebApiDevInfoClass::onDevInfoStatus(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentialsReadonly(request)) {
        return;
    }

    AsyncJsonResponse* response = new AsyncJsonResponse();
    auto& root = response->getRoot();
    auto serial = WebApi.parseSerialFromRequest(request);
    auto inv = Hoymiles.getInverterBySerial(serial);

    if (inv != nullptr) {
        root["valid_data"] = inv->DevInfo()->getLastUpdate() > 0;
        root["fw_bootloader_version"] = inv->DevInfo()->getFwBootloaderVersion();
        root["fw_build_version"] = inv->DevInfo()->getFwBuildVersion();
        root["hw_part_number"] = inv->DevInfo()->getHwPartNumber();
        root["hw_version"] = inv->DevInfo()->getHwVersion();
        root["hw_model_name"] = inv->DevInfo()->getHwModelName();
        root["max_power"] = inv->DevInfo()->getMaxPower();
        root["fw_build_datetime"] = inv->DevInfo()->getFwBuildDateTimeStr();
        root["pdl_supported"] = inv->supportsPowerDistributionLogic();
        root["firmware_update_supported"] = isFirmwareUpdateSupported(inv);
        root["firmware_update_variant"] = getFirmwareVariant(inv);
        root["firmware_update_running"] = inv->getFirmwareUpdateRunning();
        root["firmware_update_progress"] = inv->getFirmwareUpdateProgress();
        root["firmware_update_result"] = firmwareUpdateResultToString(inv->getFirmwareUpdateResult());
    }

    WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
}

void WebApiDevInfoClass::onFirmwareUpdateStart(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentials(request)) {
        return;
    }

    AsyncJsonResponse* response = new AsyncJsonResponse();
    auto& retMsg = response->getRoot();
    const uint64_t serial = WebApi.parseSerialFromRequest(request);

    if (serial == 0) {
        retMsg["type"] = "danger";
        retMsg["message"] = "Serial must be a number > 0!";
        retMsg["code"] = WebApiError::InverterSerialZero;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    auto inv = Hoymiles.getInverterBySerial(serial);
    if (inv == nullptr) {
        retMsg["type"] = "danger";
        retMsg["message"] = "Invalid inverter specified!";
        retMsg["code"] = WebApiError::PowerInvalidInverter;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    const uint8_t* rawAscii = nullptr;
    size_t rawAsciiLen = 0;
    const esp_partition_t* otaPartition = nullptr;
    size_t otaLen = 0;
    if (!pickFirmwareSource(rawAscii, rawAsciiLen, otaPartition, otaLen)) {
        retMsg["type"] = "danger";
        retMsg["message"] = "No firmware image has been uploaded!";
        retMsg["code"] = WebApiError::GenericInternalServerError;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    String mismatchReason;
    if (!firmwareFileMatchesInverter(inv, rawAscii, rawAsciiLen, otaPartition, otaLen, mismatchReason)) {
        retMsg["type"] = "danger";
        retMsg["message"] = mismatchReason;
        retMsg["code"] = WebApiError::GenericInternalServerError;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    if (!inv->sendFirmwareUpdateRequest(rawAscii, rawAsciiLen, otaPartition, otaLen)) {
        retMsg["type"] = "danger";
        retMsg["message"] = "Update could not be started!";
        retMsg["code"] = WebApiError::GenericInternalServerError;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    retMsg["type"] = "success";
    retMsg["message"] = "Update started!";
    retMsg["code"] = WebApiError::GenericSuccess;

    WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
}

void WebApiDevInfoClass::onFirmwareUpdateAbort(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentials(request)) {
        return;
    }

    AsyncJsonResponse* response = new AsyncJsonResponse();
    auto& retMsg = response->getRoot();
    const uint64_t serial = WebApi.parseSerialFromRequest(request);

    if (serial == 0) {
        retMsg["type"] = "danger";
        retMsg["message"] = "Serial must be a number > 0!";
        retMsg["code"] = WebApiError::InverterSerialZero;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    auto inv = Hoymiles.getInverterBySerial(serial);
    if (inv == nullptr) {
        retMsg["type"] = "danger";
        retMsg["message"] = "Invalid inverter specified!";
        retMsg["code"] = WebApiError::PowerInvalidInverter;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    inv->abortFirmwareUpdateRequest();

    retMsg["type"] = "success";
    retMsg["message"] = "Update aborted!";
    retMsg["code"] = WebApiError::GenericSuccess;

    WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
}

void WebApiDevInfoClass::onDevInfoRefresh(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentials(request)) {
        return;
    }

    AsyncJsonResponse* response = new AsyncJsonResponse();
    auto& retMsg = response->getRoot();
    const uint64_t serial = WebApi.parseSerialFromRequest(request);

    if (serial == 0) {
        retMsg["type"] = "danger";
        retMsg["message"] = "Serial must be a number > 0!";
        retMsg["code"] = WebApiError::InverterSerialZero;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    auto inv = Hoymiles.getInverterBySerial(serial);
    if (inv == nullptr) {
        retMsg["type"] = "danger";
        retMsg["message"] = "Invalid inverter specified!";
        retMsg["code"] = WebApiError::PowerInvalidInverter;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    if (inv->getFirmwareUpdateRunning()) {
        retMsg["type"] = "warning";
        retMsg["message"] = "Firmware update is running!";
        retMsg["code"] = WebApiError::GenericInternalServerError;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    inv->DevInfo()->invalidate();

    if (!inv->sendDevInfoRequest(true)) {
        retMsg["type"] = "danger";
        retMsg["message"] = "Device info request could not be sent!";
        retMsg["code"] = WebApiError::GenericInternalServerError;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    retMsg["type"] = "success";
    retMsg["message"] = "Device info refresh requested!";
    retMsg["code"] = WebApiError::GenericSuccess;

    WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
}
