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
#include <LittleFS.h>
#include <ctime>
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
//   [4]=const 0x10  [5]=channel code (0x10=1T 0x11=2T 0x12=4T 0x13=6T)
//   [6]=DSP flag (0x00 or 0x10)  [7]=line type (0=MI, 1=B, see
//   kFirmwareLineTypeRules)  [8..9]=fw version (BE)
// 0x13/6T is the only channel count that exists exclusively on three-phase
// (HMT) hardware -- no single-phase HM/HMS 6T model exists, so it's an
// unambiguous signal. HMT *-4T is NOT covered here: 4T also exists as a
// single-phase HM/HMS model with the same channel code, and no real capture
// is available yet to tell the two apart, so it intentionally still fails
// to match (see firmwareFileMatchesInverter()).
struct FirmwareRowRule {
    uint8_t channelCode;
    uint8_t dsp;
    uint8_t channelCount;
};

constexpr FirmwareRowRule kFirmwareRowRules[] = {
    // channelCode, dsp, channelCount
    { 0x10, 0x00, 1 }, // HM/HMS *-1T
    { 0x11, 0x00, 2 }, // HM *-2T
    { 0x11, 0x10, 2 }, // HMS *-2T
    { 0x12, 0x00, 4 }, // HM *-4T
    { 0x12, 0x10, 4 }, // HMS *-4T
    { 0x13, 0x00, 6 }, // HMT *-6T
};

// Inverter serial-number prefix ("preSerial", top 16 bits of the upper
// 32-bit half -- see e.g. HMS_1CH::isValidSerial()) mapped to the expected
// value of the row's line-type byte [7]. Disambiguates sub-variants that
// share the same channelCode/dsp (e.g. HMS_1CH vs HMS_1CHv2, both 0x10/0x00)
// which hw_model_name alone doesn't always reveal.
struct FirmwareLineTypeRule {
    uint16_t preSerial;
    uint8_t lineType;
};

constexpr FirmwareLineTypeRule kFirmwareLineTypeRules[] = {
    { 0x1124, 0 }, // MI
    { 0x1125, 1 }, // B
    { 0x1140, 1 }, // B
};

bool lookupExpectedLineType(const uint64_t serial, uint8_t& outLineType)
{
    const uint16_t preSerial = static_cast<uint16_t>((serial >> 32) & 0xffff);
    for (const auto& rule : kFirmwareLineTypeRules) {
        if (rule.preSerial == preSerial) {
            outLineType = rule.lineType;
            return true;
        }
    }
    return false;
}

// Serial-number prefixes explicitly allowed to receive a firmware update via
// this flow at all (see lib/Hoymiles/src/inverters/README.md's per-class
// serial ranges) -- a coarser gate on top of the channel/dsp/line-type
// checks above.
constexpr uint16_t kAllowedFirmwareUpdateSerialPrefixes[] = {
    0x1121, 0x1141, 0x1161, 0x1124, 0x1400, 0x1125,
    0x1143, 0x1144, 0x1410, 0x1361, 0x1164, 0x1382,
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

bool lookupFirmwareRowChannelInfo(const uint8_t channelCode, const uint8_t dsp, uint8_t& outChannelCount)
{
    for (const auto& rule : kFirmwareRowRules) {
        if (rule.channelCode == channelCode && rule.dsp == dsp) {
            outChannelCount = rule.channelCount;
            return true;
        }
    }
    return false;
}

// Reads just the first line (up to '\n', CR tolerated) of the firmware
// source into `out`; `out` is NOT NUL-terminated, see outLen.
bool readFirstFirmwareLine(const String& fsPath, const uint8_t* rawAscii, const size_t rawAsciiLen, char* out, const size_t maxLen, size_t& outLen)
{
    outLen = 0;

    if (fsPath.length() > 0) {
        File f = LittleFS.open(fsPath, "r");
        if (!f) {
            return false;
        }
        outLen = f.readBytesUntil('\n', out, maxLen);
        f.close();
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
// same hardware the inverter actually reported (both phase count and channel
// count must agree). Sets `outReason` on any failure (shown to the user).
bool firmwareFileMatchesInverter(const std::shared_ptr<InverterAbstract>& inv,
                                 const String& fsPath,
                                 const uint8_t* rawAscii,
                                 const size_t rawAsciiLen,
                                 String& outReason)
{
    const String hwModelName = inv->typeName();
    if (!isSerialAllowedForFirmwareUpdate(inv->serial())) {
        outReason = "Firmware update is not supported for this inverter!";
        return false;
    }

    bool invIsThreePhase = false;
    uint8_t invChannelCount = 0;
    uint8_t invDsp = 0;
    if (hwModelName.isEmpty() || !parseHwModelChannelInfo(hwModelName, invIsThreePhase, invChannelCount, invDsp)) {
        outReason = "Inverter hardware model is not known yet (no device info received)!";
        return false;
    }

    char lineAscii[64];
    size_t lineLen = 0;
    if (!readFirstFirmwareLine(fsPath, rawAscii, rawAsciiLen, lineAscii, sizeof(lineAscii), lineLen)) {
        outReason = "Firmware file could not be read!";
        return false;
    }

    uint8_t rowBytes[32];
    size_t rowLen = 0;
    if (IntelHex::decodeRow(lineAscii, lineLen, rowBytes, rowLen) != IntelHex::RowResult::Data || rowLen < 7) {
        outReason = "Firmware file has an unrecognized identity row!";
        return false;
    }

    uint8_t fileChannelCount = 0;
    if (!lookupFirmwareRowChannelInfo(rowBytes[5], rowBytes[6], fileChannelCount)) {
        outReason = "Firmware file target hardware could not be identified!";
        return false;
    }

    // 6T is the only channel count that's exclusively three-phase (HMT) --
    // everything else in kFirmwareRowRules is single-phase (HM/HMS).
    const bool fileIsThreePhase = (fileChannelCount == 6);
    if (fileIsThreePhase != invIsThreePhase || fileChannelCount != invChannelCount || rowBytes[6] != invDsp) {
        outReason = "Firmware file does not match the connected inverter model (" + hwModelName + ")!";
        return false;
    }

    uint8_t expectedLineType = 0;
    if (lookupExpectedLineType(inv->serial(), expectedLineType) && rowBytes[7] != expectedLineType) {
        outReason = "Firmware file does not match the connected inverter model (" + hwModelName + ")!";
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
// its first identity row, checked separately): either the persistent PSRAM
// upload buffer (peeked without copy) or the uploaded .hex file on LittleFS.
// HM_Abstract streams it row by row.
bool pickFirmwareSource(String& outFsPath, const uint8_t*& outRawAscii, size_t& outRawAsciiLen)
{
    outFsPath = String();
    outRawAscii = nullptr;
    outRawAsciiLen = 0;

    size_t psramLen = 0;
    const uint8_t* psramPtr = peekFirmwareUploadInPsram(psramLen);
    if (psramPtr != nullptr && psramLen > 0) {
        outRawAscii = psramPtr;
        outRawAsciiLen = psramLen;
        return true;
    }

    static const char* const candidatePaths[] = { "/firmware/uploaded.hex", "/littlefs/firmware/uploaded.hex" };
    for (const char* path : candidatePaths) {
        if (LittleFS.exists(path)) {
            outFsPath = path;
            return true;
        }
    }

    return false;
}
} // namespace

void WebApiDevInfoClass::init(AsyncWebServer& server, Scheduler& scheduler)
{
    using std::placeholders::_1;

    server.on("/api/devinfo/status", HTTP_GET, static_cast<ArRequestHandlerFunction>(std::bind(&WebApiDevInfoClass::onDevInfoStatus, this, _1)));
    server.on("/api/devinfo/update", HTTP_POST, static_cast<ArRequestHandlerFunction>(std::bind(&WebApiDevInfoClass::onFirmwareUpdateStart, this, _1)));
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

    String fsPath;
    const uint8_t* rawAscii = nullptr;
    size_t rawAsciiLen = 0;
    if (!pickFirmwareSource(fsPath, rawAscii, rawAsciiLen)) {
        retMsg["type"] = "danger";
        retMsg["message"] = "No firmware image has been uploaded!";
        retMsg["code"] = WebApiError::GenericInternalServerError;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    String mismatchReason;
    if (!firmwareFileMatchesInverter(inv, fsPath, rawAscii, rawAsciiLen, mismatchReason)) {
        retMsg["type"] = "danger";
        retMsg["message"] = mismatchReason;
        retMsg["code"] = WebApiError::GenericInternalServerError;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    if (!inv->sendFirmwareUpdateRequest(fsPath, rawAscii, rawAsciiLen)) {
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
