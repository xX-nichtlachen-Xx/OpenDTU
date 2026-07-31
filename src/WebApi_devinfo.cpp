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

namespace {
// Reference gateway firmware (usart_nrf.h InverterType enum) treats
// Inverter_HM_OneToOne/OneToTwo/OneToFour identically for the DOWN_PRO
// firmware-download flow (all >= Inverter_Pro -> same 16-byte chunk size,
// per-row CRC16 scheme), so all three HM channel counts are supported here.
String getFirmwareVariant(const std::shared_ptr<InverterAbstract>& inv)
{
    if (inv == nullptr) {
        return "unsupported";
    }

    const String typeName = inv->typeName();
    if (typeName.indexOf("HM-300/350/400-1T") >= 0) {
        return "1in1";
    }
    if (typeName.indexOf("HM-600/700/800-2T") >= 0) {
        return "2in1";
    }
    if (typeName.indexOf("HM-1000/1200/1500-4T") >= 0) {
        return "4in1";
    }

    return "unsupported";
}

bool isFirmwareUpdateSupported(const std::shared_ptr<InverterAbstract>& inv)
{
    return getFirmwareVariant(inv) != "unsupported";
}

// Picks the on-the-fly firmware source for the inverter without decoding
// anything: either the persistent PSRAM upload buffer (peeked without copy)
// or a .hex file path on LittleFS. HM_Abstract streams it row by row.
bool pickFirmwareSourceForInverter(const std::shared_ptr<InverterAbstract>& inv,
                                   String& outFsPath,
                                   const uint8_t*& outRawAscii,
                                   size_t& outRawAsciiLen)
{
    outFsPath = String();
    outRawAscii = nullptr;
    outRawAsciiLen = 0;

    const String variant = getFirmwareVariant(inv);
    if (variant == "unsupported") {
        return false;
    }

    size_t psramLen = 0;
    const uint8_t* psramPtr = peekFirmwareUploadInPsram(psramLen);
    if (psramPtr != nullptr && psramLen > 0) {
        outRawAscii = psramPtr;
        outRawAsciiLen = psramLen;
        return true;
    }

    String path = "/firmware/" + variant + ".hex";
    if (LittleFS.exists(path)) {
        outFsPath = path;
        return true;
    }
    path = "/littlefs/firmware/" + variant + ".hex";
    if (LittleFS.exists(path)) {
        outFsPath = path;
        return true;
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
        retMsg["message"] = "Serial must be a number > 0!";
        retMsg["code"] = WebApiError::InverterSerialZero;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    auto inv = Hoymiles.getInverterBySerial(serial);
    if (inv == nullptr) {
        retMsg["message"] = "Invalid inverter specified!";
        retMsg["code"] = WebApiError::PowerInvalidInverter;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    String fsPath;
    const uint8_t* rawAscii = nullptr;
    size_t rawAsciiLen = 0;
    if (!pickFirmwareSourceForInverter(inv, fsPath, rawAscii, rawAsciiLen)) {
        retMsg["message"] = "Firmware image is not available for this inverter type!";
        retMsg["code"] = WebApiError::GenericInternalServerError;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    if (!inv->sendFirmwareUpdateRequest(fsPath, rawAscii, rawAsciiLen)) {
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
        retMsg["message"] = "Serial must be a number > 0!";
        retMsg["code"] = WebApiError::InverterSerialZero;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    auto inv = Hoymiles.getInverterBySerial(serial);
    if (inv == nullptr) {
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
