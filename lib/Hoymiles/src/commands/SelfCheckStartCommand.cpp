#include "SelfCheckStartCommand.h"
#include "inverters/InverterAbstract.h"
#include <HoymilesUtils.h>
#include <esp_log.h>

#undef TAG
static const char* TAG = "hoymiles";

#define CRC_SIZE 6

SelfCheckStartCommand::SelfCheckStartCommand(InverterAbstract* inv, const uint64_t router_address)
    : DevControlCommand(inv, router_address)
{
    _payload[10] = 0x28;
    _payload[11] = 0x00;
    _payload[12] = 0x00;
    _payload[13] = 0x7B;
    _payload[14] = 0x00;
    _payload[15] = 0x7B;

    udpateCRC(CRC_SIZE);

    _payload_size = 18;

    setTimeout(2000);
}

String SelfCheckStartCommand::getCommandName() const
{
    return "SelfCheckStart";
}

bool SelfCheckStartCommand::handleResponse(const fragment_t fragment[], const uint8_t max_fragment_id)
{
    if (!DevControlCommand::handleResponse(fragment, max_fragment_id)) {
        return false;
    }

    if (fragment[0].len < 4 || fragment[0].fragment[2] != 0x28) {
        ESP_LOGW(TAG, "SelfCheck %s: start not executed, ACK %s belongs to an earlier command",
            _inv->serialString().c_str(), HoymilesUtils::dumpArray(fragment[0].fragment, fragment[0].len).c_str());
        _inv->SelfCheck()->setStartResult(false);
        return true;
    }

    if (fragment[0].fragment[0] == 0x04 && fragment[0].fragment[1] == 0x00) {
        ESP_LOGW(TAG, "SelfCheck %s: not supported by inverter, ACK %s",
            _inv->serialString().c_str(), HoymilesUtils::dumpArray(fragment[0].fragment, fragment[0].len).c_str());
        _inv->SelfCheck()->setUnsupported();
        return true;
    }

    ESP_LOGI(TAG, "SelfCheck %s: start ACK", _inv->serialString().c_str());
    _inv->SelfCheck()->setStartResult(true);
    return true;
}

void SelfCheckStartCommand::gotTimeout()
{
    ESP_LOGW(TAG, "SelfCheck %s: start NOK (timeout)", _inv->serialString().c_str());
    _inv->SelfCheck()->setStartResult(false);
}
