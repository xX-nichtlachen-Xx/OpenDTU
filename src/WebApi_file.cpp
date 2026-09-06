// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022-2026 Thomas Basler and others
 */
#include "WebApi_file.h"
#include "Configuration.h"
#include "RestartHelper.h"
#include "Utils.h"
#include "WebApi.h"
#include "WebApi_errors.h"
#include <Arduino.h>
#include <AsyncJson.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <cinttypes>
#include <cstring>

#undef TAG
static const char* TAG = "webapi";

namespace {
constexpr size_t MAX_FIRMWARE_UPLOAD_SIZE = 800 * 1024;

struct PsramFirmwareUploadBuffer {
    uint8_t* data = nullptr;
    size_t size = 0;
    size_t capacity = 0;
    String variant;
};

PsramFirmwareUploadBuffer g_psramFirmwareUploadBuffer;

// OTA-slot fallback (used when no PSRAM is present): the image is written
// straight to flash chunk-by-chunk and NEVER buffered in RAM -- keeping a
// second full copy in the (tiny, no-PSRAM) internal heap is exactly what
// caused out-of-memory aborts during upload on non-PSRAM boards.
struct OtaSlotFirmwareUpload {
    size_t size = 0;
    String variant;
};
OtaSlotFirmwareUpload g_otaFirmwareUpload;

const esp_partition_t* getInactiveFirmwarePartition()
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running == nullptr) {
        ESP_LOGE(TAG, "FW upload: could not determine running OTA partition");
        return nullptr;
    }

    const esp_partition_t* target = esp_ota_get_next_update_partition(running);
    if (target == nullptr) {
        ESP_LOGE(TAG, "FW upload: no inactive OTA partition available (running=\"%s\")", running->label);
    } else {
        ESP_LOGD(TAG, "FW upload: target inactive OTA partition \"%s\" (offset=0x%06" PRIx32 ", size=%" PRIu32 ")",
            target->label, target->address, target->size);
    }
    return target;
}

String normalizeUploadPath(const String& value)
{
    String path = value;
    path.trim();
    path.replace('\\', '/');

    if (path.startsWith("/")) {
        path.remove(0, 1);
    }

    if (path.length() == 0 || path.indexOf("..") >= 0) {
        return "";
    }

    return "/" + path;
}

bool ensureParentDirectories(const String& path)
{
    if (path.length() <= 1) {
        return true;
    }

    const int slashPos = path.lastIndexOf('/');
    if (slashPos <= 0) {
        return true;
    }

    String parent = path.substring(0, slashPos);
    String current = "/";
    int start = 1;

    while (start < parent.length()) {
        const int next = parent.indexOf('/', start);
        const String segment = next >= 0 ? parent.substring(start, next) : parent.substring(start);
        if (segment.length() == 0) {
            break;
        }

        current += segment;
        if (!LittleFS.exists(current)) {
            if (!LittleFS.mkdir(current)) {
                return false;
            }
        }

        if (next < 0) {
            break;
        }
        current += "/";
        start = next + 1;
    }

    return true;
}
} // namespace

bool writeFirmwareUploadToPsram(const uint8_t* data, size_t len, const String& variant)
{
    if (data == nullptr || len == 0) {
        if (!variant.isEmpty()) {
            g_psramFirmwareUploadBuffer.variant = variant;
        }
        return true;
    }

    if (g_psramFirmwareUploadBuffer.size + len > MAX_FIRMWARE_UPLOAD_SIZE) {
        ESP_LOGE(TAG, "FW upload (PSRAM): total size would exceed %u bytes limit (have %u, +%u)",
            static_cast<unsigned>(MAX_FIRMWARE_UPLOAD_SIZE), static_cast<unsigned>(g_psramFirmwareUploadBuffer.size), static_cast<unsigned>(len));
        return false;
    }

    if (g_psramFirmwareUploadBuffer.capacity < g_psramFirmwareUploadBuffer.size + len) {
        if (ESP.getPsramSize() == 0) {
            ESP_LOGE(TAG, "FW upload (PSRAM): no PSRAM present, cannot grow buffer");
            return false;
        }

        size_t newCapacity = g_psramFirmwareUploadBuffer.capacity == 0 ? len : g_psramFirmwareUploadBuffer.capacity;
        while (newCapacity < g_psramFirmwareUploadBuffer.size + len) {
            newCapacity *= 2;
        }

        if (ESP.getFreePsram() < newCapacity) {
            ESP_LOGE(TAG, "FW upload (PSRAM): not enough free PSRAM (need %u, free %u)",
                static_cast<unsigned>(newCapacity), static_cast<unsigned>(ESP.getFreePsram()));
            return false;
        }

        uint8_t* newBuffer = static_cast<uint8_t*>(heap_caps_malloc(newCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (newBuffer == nullptr) {
            ESP_LOGE(TAG, "FW upload (PSRAM): heap_caps_malloc(%u) failed", static_cast<unsigned>(newCapacity));
            return false;
        }
        ESP_LOGD(TAG, "FW upload (PSRAM): grew buffer capacity %u -> %u bytes",
            static_cast<unsigned>(g_psramFirmwareUploadBuffer.capacity), static_cast<unsigned>(newCapacity));

        if (g_psramFirmwareUploadBuffer.data != nullptr) {
            memcpy(newBuffer, g_psramFirmwareUploadBuffer.data, g_psramFirmwareUploadBuffer.size);
            heap_caps_free(g_psramFirmwareUploadBuffer.data);
        }

        g_psramFirmwareUploadBuffer.data = newBuffer;
        g_psramFirmwareUploadBuffer.capacity = newCapacity;
    }

    if (!variant.isEmpty()) {
        g_psramFirmwareUploadBuffer.variant = variant;
    }

    memcpy(g_psramFirmwareUploadBuffer.data + g_psramFirmwareUploadBuffer.size, data, len);
    g_psramFirmwareUploadBuffer.size += len;
    ESP_LOGD(TAG, "FW upload (PSRAM): wrote chunk of %u bytes, total %u bytes",
        static_cast<unsigned>(len), static_cast<unsigned>(g_psramFirmwareUploadBuffer.size));
    return true;
}

bool getFirmwareUploadFromPsram(std::vector<uint8_t>& buffer)
{
    if (g_psramFirmwareUploadBuffer.size == 0 || g_psramFirmwareUploadBuffer.data == nullptr) {
        return false;
    }

    buffer.assign(g_psramFirmwareUploadBuffer.data, g_psramFirmwareUploadBuffer.data + g_psramFirmwareUploadBuffer.size);
    return true;
}

const uint8_t* peekFirmwareUploadInPsram(size_t& outLen)
{
    if (g_psramFirmwareUploadBuffer.size == 0 || g_psramFirmwareUploadBuffer.data == nullptr) {
        outLen = 0;
        return nullptr;
    }
    outLen = g_psramFirmwareUploadBuffer.size;
    return g_psramFirmwareUploadBuffer.data;
}

String getFirmwareUploadVariant()
{
    return g_psramFirmwareUploadBuffer.variant;
}

void setFirmwareUploadVariant(const String& variant)
{
    g_psramFirmwareUploadBuffer.variant = variant;
}

void clearFirmwareUploadFromPsram()
{
    ESP_LOGD(TAG, "FW upload (PSRAM): clearing buffer (previous size %u)", static_cast<unsigned>(g_psramFirmwareUploadBuffer.size));
    if (g_psramFirmwareUploadBuffer.data != nullptr) {
        heap_caps_free(g_psramFirmwareUploadBuffer.data);
    }
    g_psramFirmwareUploadBuffer.data = nullptr;
    g_psramFirmwareUploadBuffer.size = 0;
    g_psramFirmwareUploadBuffer.capacity = 0;
    g_psramFirmwareUploadBuffer.variant = String();
}

bool writeFirmwareUploadToInactiveOtaSlot(const uint8_t* data, size_t len, const String& variant)
{
    if (data == nullptr || len == 0) {
        if (!variant.isEmpty()) {
            g_otaFirmwareUpload.variant = variant;
        }
        return true;
    }

    const esp_partition_t* partition = getInactiveFirmwarePartition();
    if (partition == nullptr) {
        return false;
    }

    if (len > MAX_FIRMWARE_UPLOAD_SIZE) {
        ESP_LOGE(TAG, "FW upload (OTA slot): chunk of %u bytes exceeds %u bytes limit",
            static_cast<unsigned>(len), static_cast<unsigned>(MAX_FIRMWARE_UPLOAD_SIZE));
        return false;
    }

    if (g_otaFirmwareUpload.size == 0) {
        ESP_LOGD(TAG, "FW upload (OTA slot): erasing partition \"%s\" (%" PRIu32 " bytes)", partition->label, partition->size);
        const esp_err_t eraseResult = esp_partition_erase_range(partition, 0, partition->size);
        if (eraseResult != ESP_OK) {
            ESP_LOGE(TAG, "FW upload (OTA slot): esp_partition_erase_range failed: %s", esp_err_to_name(eraseResult));
            return false;
        }
    }

    if (g_otaFirmwareUpload.size + len > partition->size) {
        ESP_LOGE(TAG, "FW upload (OTA slot): total size would exceed partition size %" PRIu32 " (have %u, +%u)",
            partition->size, static_cast<unsigned>(g_otaFirmwareUpload.size), static_cast<unsigned>(len));
        return false;
    }

    const esp_err_t writeResult = esp_partition_write(partition, g_otaFirmwareUpload.size, data, len);
    if (writeResult != ESP_OK) {
        ESP_LOGE(TAG, "FW upload (OTA slot): esp_partition_write failed at offset %u, len %u: %s",
            static_cast<unsigned>(g_otaFirmwareUpload.size), static_cast<unsigned>(len), esp_err_to_name(writeResult));
        return false;
    }

    if (!variant.isEmpty()) {
        g_otaFirmwareUpload.variant = variant;
    }

    g_otaFirmwareUpload.size += len;
    ESP_LOGD(TAG, "FW upload (OTA slot): wrote chunk of %u bytes, total %u bytes",
        static_cast<unsigned>(len), static_cast<unsigned>(g_otaFirmwareUpload.size));
    return true;
}

bool getFirmwareUploadInInactiveOtaSlot(const esp_partition_t*& outPartition, size_t& outLen)
{
    if (g_otaFirmwareUpload.size == 0) {
        outPartition = nullptr;
        outLen = 0;
        return false;
    }

    outPartition = getInactiveFirmwarePartition();
    outLen = g_otaFirmwareUpload.size;
    return outPartition != nullptr;
}

void clearFirmwareUploadFromInactiveOtaSlot()
{
    ESP_LOGD(TAG, "FW upload (OTA slot): clearing buffer (previous size %u)", static_cast<unsigned>(g_otaFirmwareUpload.size));
    const esp_partition_t* partition = getInactiveFirmwarePartition();
    if (partition != nullptr) {
        esp_partition_erase_range(partition, 0, partition->size);
    }

    g_otaFirmwareUpload.size = 0;
    g_otaFirmwareUpload.variant = String();
}

void WebApiFileClass::init(AsyncWebServer& server, Scheduler& scheduler)
{
    using std::placeholders::_1;
    using std::placeholders::_2;
    using std::placeholders::_3;
    using std::placeholders::_4;
    using std::placeholders::_5;
    using std::placeholders::_6;

    server.on("/api/file/get", HTTP_GET, static_cast<ArRequestHandlerFunction>(std::bind(&WebApiFileClass::onFileGet, this, _1)));
    server.on("/api/file/delete", HTTP_POST, static_cast<ArRequestHandlerFunction>(std::bind(&WebApiFileClass::onFileDelete, this, _1)));
    server.on("/api/file/delete_all", HTTP_POST, static_cast<ArRequestHandlerFunction>(std::bind(&WebApiFileClass::onFileDeleteAll, this, _1)));
    server.on("/api/file/list", HTTP_GET, static_cast<ArRequestHandlerFunction>(std::bind(&WebApiFileClass::onFileListGet, this, _1)));
    server.on("/api/file/upload", HTTP_POST,
        std::bind(&WebApiFileClass::onFileUploadFinish, this, _1),
        std::bind(&WebApiFileClass::onFileUpload, this, _1, _2, _3, _4, _5, _6));
}

void WebApiFileClass::onFileListGet(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentials(request)) {
        return;
    }

    AsyncJsonResponse* response = new AsyncJsonResponse();
    auto& root = response->getRoot();
    auto data = root.to<JsonArray>();

    File rootfs = LittleFS.open("/");
    File file = rootfs.openNextFile();
    while (file) {
        if (file.isDirectory()) {
            continue;
        }
        JsonObject obj = data.add<JsonObject>();
        obj["name"] = String(file.name());
        obj["size"] = file.size();

        file = rootfs.openNextFile();
    }
    file.close();

    WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
}

void WebApiFileClass::onFileGet(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentials(request)) {
        return;
    }

    String requestFile = CONFIG_FILENAME;
    if (request->hasParam("file")) {
        String name = "/" + request->getParam("file")->value();
        if (LittleFS.exists(name)) {
            requestFile = name;
        } else {
            request->send(404);
            return;
        }
    }

    request->send(LittleFS, requestFile, String(), true);
}

void WebApiFileClass::onFileDelete(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentials(request)) {
        return;
    }

    AsyncJsonResponse* response = new AsyncJsonResponse();
    JsonDocument root;
    if (!WebApi.parseRequestData(request, response, root)) {
        return;
    }

    auto& retMsg = response->getRoot();

    if (!(root["file"].is<String>())) {
        retMsg["message"] = "Values are missing!";
        retMsg["code"] = WebApiError::GenericValueMissing;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    String name = "/" + root["file"].as<String>();
    if (!LittleFS.exists(name)) {
        request->send(404);
        return;
    }

    LittleFS.remove(name);

    retMsg["type"] = "success";
    retMsg["message"] = "File deleted";
    retMsg["code"] = WebApiError::FileDeleteSuccess;

    WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
}

void WebApiFileClass::onFileDeleteAll(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentials(request)) {
        return;
    }

    AsyncJsonResponse* response = new AsyncJsonResponse();
    JsonDocument root;
    if (!WebApi.parseRequestData(request, response, root)) {
        return;
    }

    auto& retMsg = response->getRoot();

    if (!(root["delete"].is<bool>())) {
        retMsg["message"] = "Values are missing!";
        retMsg["code"] = WebApiError::GenericValueMissing;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    if (root["delete"].as<bool>() == false) {
        retMsg["message"] = "Not deleted anything!";
        retMsg["code"] = WebApiError::FileNotDeleted;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    retMsg["type"] = "success";
    retMsg["message"] = "Configuration resettet. Rebooting now...";
    retMsg["code"] = WebApiError::FileSuccess;

    WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);

    Utils::removeAllFiles();
    RestartHelper.triggerRestart();
}

void WebApiFileClass::onFileUpload(AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final)
{
    if (!WebApi.checkCredentials(request)) {
        return;
    }

    if (!index) {
        // open the file on first call and store the file handle in the request object
        if (!request->hasParam("file")) {
            request->send(500);
            return;
        }

        const String fileParam = request->getParam("file")->value();
        const String name = normalizeUploadPath(fileParam);
        const bool usePsram = name.startsWith("/firmware/") && ESP.getPsramSize() > 0;
        const bool useInactiveOtaSlot = name.startsWith("/firmware/") && ESP.getPsramSize() == 0;
        request->setAttribute("upload_use_psram", usePsram);
        request->setAttribute("upload_use_ota_slot", useInactiveOtaSlot);
        ESP_LOGI(TAG, "FW upload: starting upload of \"%s\" -> \"%s\" (psram=%d, otaSlot=%d, psramSize=%u)",
            fileParam.c_str(), name.c_str(), usePsram, useInactiveOtaSlot, static_cast<unsigned>(ESP.getPsramSize()));

        if (usePsram) {
            clearFirmwareUploadFromPsram();
            const int slashPos = name.lastIndexOf('/');
            const int dotPos = name.lastIndexOf('.');
            if (slashPos >= 0 && dotPos > slashPos) {
                setFirmwareUploadVariant(name.substring(slashPos + 1, dotPos));
            }
            // NOTE: do NOT return here -- this first callback invocation
            // already carries the first (and for small files, the only)
            // chunk of data. Returning early discarded that chunk, leaving
            // the PSRAM buffer empty and causing loadFirmwareForInverter()
            // to silently fall back to a stale/unrelated file on LittleFS.
        } else if (useInactiveOtaSlot) {
            clearFirmwareUploadFromInactiveOtaSlot();
            const int slashPos = name.lastIndexOf('/');
            const int dotPos = name.lastIndexOf('.');
            if (slashPos >= 0 && dotPos > slashPos) {
                setFirmwareUploadVariant(name.substring(slashPos + 1, dotPos));
            }
        } else {
            if (name.length() == 0 || !ensureParentDirectories(name)) {
                ESP_LOGE(TAG, "FW upload: invalid path or failed to create parent directories for \"%s\"", name.c_str());
                request->send(500);
                return;
            }

            request->_tempFile = LittleFS.open(name, "w");
            if (!request->_tempFile) {
                ESP_LOGE(TAG, "FW upload: LittleFS.open(\"%s\", \"w\") failed", name.c_str());
                request->send(500);
                return;
            }
        }
    }

    // Cached as a request attribute on the first (!index) callback above, so
    // later chunk/finalization calls for the same upload don't need to
    // re-derive it from the "file" param each time.
    const bool usePsram = request->getAttribute("upload_use_psram", false);
    const bool useInactiveOtaSlot = request->getAttribute("upload_use_ota_slot", false);

    if (len) {
        ESP_LOGD(TAG, "FW upload: chunk index=%u len=%u final=%d", static_cast<unsigned>(index), static_cast<unsigned>(len), final);
        if (usePsram) {
            String variant = getFirmwareUploadVariant();
            if (!writeFirmwareUploadToPsram(data, len, variant)) {
                // Don't leave a partial image behind for a later request to
                // mistake for a complete upload.
                ESP_LOGE(TAG, "FW upload (PSRAM): write failed at index=%u len=%u, aborting upload", static_cast<unsigned>(index), static_cast<unsigned>(len));
                clearFirmwareUploadFromPsram();
                request->send(500);
                return;
            }
        } else if (useInactiveOtaSlot) {
            String variant = getFirmwareUploadVariant();
            if (!writeFirmwareUploadToInactiveOtaSlot(data, len, variant)) {
                ESP_LOGE(TAG, "FW upload (OTA slot): write failed at index=%u len=%u, aborting upload", static_cast<unsigned>(index), static_cast<unsigned>(len));
                clearFirmwareUploadFromInactiveOtaSlot();
                request->send(500);
                return;
            }
        } else {
            // stream the incoming chunk to the opened file
            request->_tempFile.write(data, len);
        }
    }

    if (final && usePsram) {
        // Only reachable once every chunk, including this last one, was
        // written successfully -- onFileUploadFinish relies on this instead
        // of re-deriving success from the buffer/variant state.
        ESP_LOGI(TAG, "FW upload (PSRAM): upload finished successfully, total %u bytes", static_cast<unsigned>(index + len));
        request->setAttribute("upload_psram_complete", true);
    }

    if (final && useInactiveOtaSlot) {
        ESP_LOGI(TAG, "FW upload (OTA slot): upload finished successfully, total %u bytes", static_cast<unsigned>(index + len));
        request->setAttribute("upload_ota_slot_complete", true);
    }

    if (final && !usePsram && !useInactiveOtaSlot) {
        // close the file handle as the upload is now done
        ESP_LOGI(TAG, "FW upload (LittleFS): upload finished successfully, total %u bytes", static_cast<unsigned>(index + len));
        request->_tempFile.close();
    }
}

void WebApiFileClass::onFileUploadFinish(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentials(request)) {
        return;
    }

    // the request handler is triggered after the upload has finished...
    // create the response, add header, and send response

    bool restart = true;
    if (request->hasParam("restart")) {
        const String restartValue = request->getParam("restart")->value();
        restart = restartValue.equalsIgnoreCase("1") || restartValue.equalsIgnoreCase("true");
    }

    bool uploadSucceeded = true;
    if (request->hasParam("file")) {
        const String fileParam = request->getParam("file")->value();
        const String name = normalizeUploadPath(fileParam);
        if (name.startsWith("/firmware/")) {
            const bool psramComplete = request->getAttribute("upload_psram_complete", false);
            const bool otaSlotComplete = request->getAttribute("upload_ota_slot_complete", false);
            uploadSucceeded = psramComplete || otaSlotComplete;
            ESP_LOGI(TAG, "FW upload: finish handler for \"%s\", success=%d (psramComplete=%d, otaSlotComplete=%d)",
                name.c_str(), uploadSucceeded, psramComplete, otaSlotComplete);
        }
    }

    AsyncWebServerResponse* response = request->beginResponse(uploadSucceeded ? 200 : 500, asyncsrv::T_text_plain,
        uploadSucceeded ? "OK" : "Firmware upload failed");
    response->addHeader(asyncsrv::T_Connection, asyncsrv::T_close);
    response->addHeader(asyncsrv::T_CORS_ACAO, "*");
    request->send(response);

    if (uploadSucceeded && restart) {
        RestartHelper.triggerRestart();
    }
}
