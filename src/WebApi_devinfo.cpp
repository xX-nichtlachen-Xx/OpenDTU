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

// The first Intel-Hex-style line of every known firmware image is a fixed
// vendor-specific "identity" row (not real flash data) encoding the target
// hardware. Verified against the real single-phase (HM/HMS) images in
// Firmware/*.hex: after IntelHex::decodeRow() strips the line's own trailing
// checksum byte, the row is exactly 10 bytes:
//   [0]=LL(0x06)  [1..2]=address(0x0000)  [3]=record type(0x11)
//   [4]=const 0x10  [5]=channel code (0x10 | inputType, see
//   kFirmwareSerialRules)  [6]=DSP flag  [7]=line type (0=MI, 1=B)
//   [8..9]=fw version (BE)
// Per-inverter values below (phase/inputType/dsp/lineType) come straight
// from captured firmware identity rows, keyed by the inverter serial-number
// prefix ("preSerial", top 16 bits of the upper 32-bit half -- see e.g.
// HMS_1CH::isValidSerial()), since hw_model_name alone doesn't always
// disambiguate sub-variants sharing the same channel/dsp encoding.
// inputType 5/6 (HMT *-4T) have no confirmed firmware capture yet, so they
// intentionally never match a real row (see firmwareFileMatchesInverter()).
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
    // byte [4] = 0x1? ; ignore high nibble, compare low nibble as newGen1
    if ((rowBytes[0] & 0x0F) != rule.newGen1) {
        return false;
    }

    // byte [5] = phase << 4 | inputType
    if ((rowBytes[1] >> 4) != rule.phase || (rowBytes[1] & 0x0F) != rule.inputType) {
        return false;
    }

    // byte [6] = dsp << 4 | newGen2
    if ((rowBytes[2] >> 4) != rule.dsp || (rowBytes[2] & 0x0F) != rule.newGen2) {
        return false;
    }

    // byte [7] = newGen3 << 4 | bType
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

// Serial-number prefixes explicitly allowed to receive a firmware update via
// this flow at all (see lib/Hoymiles/src/inverters/README.md's per-class
// serial ranges) -- a coarser gate on top of the channel/dsp/line-type
// checks above.
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

// Extracts the phase/channel information encoded in the trailing "-<N>T" of
// the hardware-REPORTED model name e.g.
// "HMS-1800-4T" -> 4, "HMT-2250-6T" -> 6), three-phase iff the name starts
// with "HMT-". Returns false if no such suffix is found (e.g. HERF-* names).
// `outDsp` follows the row's raw encoding (0x00/0x10): HMS models use 0x10,
// EXCEPT the *-1T variant (shared with HM), which -- like all other HM/HMT
// models -- uses 0x00.
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

// Reads just the first line (up to '\n') of the firmware
// source into `out`; `out` is NOT NUL-terminated, see outLen.
bool readFirstFirmwareLine(const uint8_t* rawAscii, const size_t rawAsciiLen,
                           const esp_partition_t* otaPartition, const size_t otaLen,
                           char* out, const size_t maxLen, size_t& outLen)
{
    outLen = 0;

    if (otaPartition != nullptr && otaLen > 0) {
        // Streams straight from flash one byte at a time -- this is only
        // ever called for the short (<64 byte) identity row, so the extra
        // esp_partition_read() call overhead per byte is negligible, and it
        // avoids ever buffering the (potentially large) firmware image in RAM.
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

// Parses the firmware source's first line and reports whether it targets the
// same hardware the inverter actually reported (looked up by serial via
// kFirmwareSerialRules). Sets `outReason` on any failure (shown to the user).
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
    // typeName() is derived from the serial alone, so it's known immediately
    // (unlike the DevInfo hw_model_name, which requires a device-info poll).
    bool isThreePhase = false;
    uint8_t channelCount = 0;
    uint8_t dsp = 0;
    return parseHwModelChannelInfo(inv->typeName(), isThreePhase, channelCount, dsp);
}

// Human-readable channel descriptor derived from the serial-based type name,
// purely informational (the actual upload no longer needs a variant
// selection -- see firmwareFileMatchesInverter()).
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

// Picks the on-the-fly firmware source without decoding anything (other than
// its first identity row, checked separately): the persistent PSRAM upload
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
