// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <ESPAsyncWebServer.h>
#include <TaskSchedulerDeclarations.h>
#include <cstddef>
#include <cstdint>
#include <vector>

class WebApiFileClass {
public:
    void init(AsyncWebServer& server, Scheduler& scheduler);

private:
    void onFileGet(AsyncWebServerRequest* request);
    void onFileDelete(AsyncWebServerRequest* request);
    void onFileDeleteAll(AsyncWebServerRequest* request);
    void onFileListGet(AsyncWebServerRequest* request);
    void onFileUploadFinish(AsyncWebServerRequest* request);
    void onFileUpload(AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final);
};

bool writeFirmwareUploadToPsram(const uint8_t* data, size_t len, const String& variant = String());
bool getFirmwareUploadFromPsram(std::vector<uint8_t>& buffer);
// Direct read-only view of the persistent PSRAM upload buffer -- no copy.
// The returned pointer is valid until clearFirmwareUploadFromPsram() is
// called. Returns nullptr if no upload is stored.
const uint8_t* peekFirmwareUploadInPsram(size_t& outLen);
String getFirmwareUploadVariant();
void setFirmwareUploadVariant(const String& variant);
void clearFirmwareUploadFromPsram();

bool writeFirmwareUploadToInactiveOtaSlot(const uint8_t* data, size_t len, const String& variant = String());
bool getFirmwareUploadFromInactiveOtaSlot(std::vector<uint8_t>& buffer);
const uint8_t* peekFirmwareUploadInInactiveOtaSlot(size_t& outLen);
void clearFirmwareUploadFromInactiveOtaSlot();
