#include "WebApi_selfcheck.h"
#include "WebApi.h"
#include "WebApi_errors.h"
#include <AsyncJson.h>
#include <Hoymiles.h>

void WebApiSelfCheckClass::init(AsyncWebServer& server, Scheduler& scheduler)
{
    using std::placeholders::_1;

    server.on("/api/selfcheck/status", HTTP_GET, static_cast<ArRequestHandlerFunction>(std::bind(&WebApiSelfCheckClass::onSelfCheckStatus, this, _1)));
    server.on("/api/selfcheck/start", HTTP_POST, static_cast<ArRequestHandlerFunction>(std::bind(&WebApiSelfCheckClass::onSelfCheckStart, this, _1)));
}

void WebApiSelfCheckClass::onSelfCheckStatus(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentialsReadonly(request)) {
        return;
    }

    AsyncJsonResponse* response = new AsyncJsonResponse();
    auto& root = response->getRoot();
    auto serial = WebApi.parseSerialFromRequest(request);
    auto inv = Hoymiles.getInverterBySerial(serial);

    if (inv != nullptr) {
        auto selfCheck = inv->SelfCheck();
        const SelfCheckState state = selfCheck->getState();

        root["state"] = SelfCheckParser::getStateName(state);
        root["commands_enabled"] = inv->getEnableCommands();
        root["elapsed"] = state == SelfCheckState::Idle ? 0 : (millis() - selfCheck->getStartTime()) / 1000;
        root["polls"] = selfCheck->getPollCount();
        root["ips_state"] = selfCheck->getIpsState();
        root["ips_state_name"] = SelfCheckParser::getIpsStateName(selfCheck->getIpsState());

        char profileId[5];
        snprintf(profileId, sizeof(profileId), "%04X", selfCheck->getProfileId());
        root["profile_id"] = profileId;
        root["profile_name"] = selfCheck->getProfileName();
        root["profile_version"] = selfCheck->getProfileVersion();

        auto items = root["items"].to<JsonArray>();
        for (uint8_t i = 0; i < selfCheck->getItemCount(); i++) {
            const auto item = selfCheck->getItem(i);
            char code[5];
            snprintf(code, sizeof(code), "%04X", item.Code);

            auto jsonItem = items.add<JsonObject>();
            jsonItem["id"] = SelfCheckParser::getItemId(i);
            jsonItem["unit"] = SelfCheckParser::getItemUnit(i);
            jsonItem["code"] = code;
            jsonItem["passed"] = item.Passed;
            jsonItem["value_digits"] = item.ValueDigits;
            jsonItem["time_digits"] = item.TimeDigits;
            jsonItem["set_value"] = item.SetValue;
            jsonItem["set_time"] = item.SetTime;
            jsonItem["meas_value"] = item.MeasValue;
            jsonItem["meas_time"] = item.MeasTime;
        }

        auto fragments = root["fragments"].to<JsonArray>();
        for (const auto id : selfCheck->getReceivedFragments()) {
            fragments.add(id);
        }
        root["fragments_total"] = selfCheck->getFragmentTotal();

        auto raw = root["raw"].to<JsonArray>();
        auto data = selfCheck->getRawData();
        if (data.size() > 0) {
            copyArray(&data[0], data.size(), raw);
        }
    }

    WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
}

void WebApiSelfCheckClass::onSelfCheckStart(AsyncWebServerRequest* request)
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

    if (!root["serial"].is<String>()) {
        retMsg["message"] = "Values are missing!";
        retMsg["code"] = WebApiError::GenericValueMissing;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    const uint64_t serial = strtoll(root["serial"].as<String>().c_str(), NULL, 16);

    if (serial == 0) {
        retMsg["message"] = "Serial must be a number > 0!";
        retMsg["code"] = WebApiError::SelfCheckSerialZero;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    auto inv = Hoymiles.getInverterBySerial(serial);
    if (inv == nullptr) {
        retMsg["message"] = "Invalid inverter specified!";
        retMsg["code"] = WebApiError::SelfCheckInvalidInverter;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    if (!inv->getEnableCommands()) {
        retMsg["message"] = "Sending commands is disabled for this inverter!";
        retMsg["code"] = WebApiError::SelfCheckCommandsDisabled;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    if (inv->SelfCheck()->isActive()) {
        retMsg["message"] = "Self-check is already running!";
        retMsg["code"] = WebApiError::SelfCheckAlreadyRunning;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    inv->sendSelfCheckStartRequest();

    retMsg["type"] = "success";
    retMsg["message"] = "Self-check started!";
    retMsg["code"] = WebApiError::SelfCheckStarted;

    WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
}
