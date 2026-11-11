#pragma once

#include <ESPAsyncWebServer.h>
#include <TaskSchedulerDeclarations.h>

class WebApiSelfCheckClass {
public:
    void init(AsyncWebServer& server, Scheduler& scheduler);

private:
    void onSelfCheckStatus(AsyncWebServerRequest* request);
    void onSelfCheckStart(AsyncWebServerRequest* request);
};
