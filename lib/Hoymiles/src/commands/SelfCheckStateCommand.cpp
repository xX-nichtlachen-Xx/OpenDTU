#include "SelfCheckStateCommand.h"
#include "inverters/InverterAbstract.h"
#include <HoymilesUtils.h>
#include <esp_log.h>

#undef TAG
static const char* TAG = "hoymiles";

SelfCheckStateCommand::SelfCheckStateCommand(InverterAbstract* inv, const uint64_t router_address, const time_t time)
    : MultiDataCommand(inv, router_address)
{
    setTime(time);
    setDataType(0x1E);
    setTimeout(500);
}

String SelfCheckStateCommand::getCommandName() const
{
    return "SelfCheckState";
}

bool SelfCheckStateCommand::handleResponse(const fragment_t fragment[], const uint8_t max_fragment_id)
{
    if (!MultiDataCommand::handleResponse(fragment, max_fragment_id)) {
        return false;
    }

    uint8_t offs = 0;
    auto selfCheck = _inv->SelfCheck();
    selfCheck->beginAppendFragment();
    selfCheck->clearBuffer();
    for (uint8_t i = 0; i < max_fragment_id; i++) {
        selfCheck->appendFragment(offs, fragment[i].fragment, fragment[i].len);
        offs += (fragment[i].len);
    }
    selfCheck->endAppendFragment();
    selfCheck->evaluateBuffer();
    selfCheck->setLastUpdate(millis());

    logResult();
    return true;
}

void SelfCheckStateCommand::handlePartialResponse(const fragment_t fragment[], const uint8_t last_fragment_id, const uint8_t max_fragment_id)
{
    auto selfCheck = _inv->SelfCheck();
    if (selfCheck->addPartialFragments(fragment, last_fragment_id, max_fragment_id)) {
        selfCheck->setLastUpdate(millis());
        logResult();
        return;
    }

    if (selfCheck->getState() != SelfCheckState::Collecting) {
        return;
    }

    String received;
    for (const auto id : selfCheck->getReceivedFragments()) {
        received += String(id) + " ";
    }
    ESP_LOGW(TAG, "SelfCheck %s: test finished, result incomplete, fragments %sof %u",
        _inv->serialString().c_str(), received.c_str(), selfCheck->getFragmentTotal());
}

void SelfCheckStateCommand::logResult()
{
    auto selfCheck = _inv->SelfCheck();
    const auto raw = selfCheck->getRawData();
    const char* serial = _inv->serialString().c_str();

    if (selfCheck->getState() == SelfCheckState::Unsupported) {
        ESP_LOGW(TAG, "SelfCheck %s: not supported by inverter, raw=%s",
            serial, HoymilesUtils::dumpArray(raw.data(), raw.size()).c_str());
        return;
    }

    if (selfCheck->getState() == SelfCheckState::Reset) {
        ESP_LOGW(TAG, "SelfCheck %s: reset by inverter after %" PRIu32 " polls, raw=%s",
            serial, selfCheck->getPollCount(), HoymilesUtils::dumpArray(raw.data(), raw.size()).c_str());
        return;
    }

    if (selfCheck->getState() != SelfCheckState::Finished) {
        ESP_LOGI(TAG, "SelfCheck %s: t=%" PRIu32 "s state=%u len=%u raw=%s",
            serial, static_cast<uint32_t>((millis() - selfCheck->getStartTime()) / 1000), selfCheck->getIpsState(), static_cast<unsigned>(raw.size()),
            HoymilesUtils::dumpArray(raw.data(), raw.size()).c_str());
        return;
    }

    ESP_LOGI(TAG, "SelfCheck %s: finished after %" PRIu32 " s and %" PRIu32 " polls, IPS state %u (%s), profile %04X %s v%s, %u items, %u bytes",
        serial, static_cast<uint32_t>((millis() - selfCheck->getStartTime()) / 1000), selfCheck->getPollCount(), selfCheck->getIpsState(), SelfCheckParser::getIpsStateName(selfCheck->getIpsState()),
        selfCheck->getProfileId(), selfCheck->getProfileName().c_str(), selfCheck->getProfileVersion().c_str(),
        selfCheck->getItemCount(), static_cast<unsigned>(raw.size()));

    for (size_t offs = 0; offs < raw.size(); offs += 16) {
        const uint8_t len = (raw.size() - offs) > 16 ? 16 : (raw.size() - offs);
        ESP_LOGI(TAG, "SelfCheck %s: raw[%02u] %s", serial, static_cast<unsigned>(offs), HoymilesUtils::dumpArray(&raw[offs], len).c_str());
    }

    for (uint8_t i = 0; i < selfCheck->getItemCount(); i++) {
        const auto item = selfCheck->getItem(i);
        const char* unit = SelfCheckParser::getItemUnit(i);
        ESP_LOGI(TAG, "SelfCheck %s: %s code=%04X set=%.*f%s/%.*fs meas=%.*f%s/%.*fs %s",
            serial, SelfCheckParser::getItemId(i), item.Code,
            item.ValueDigits, item.SetValue, unit, item.TimeDigits, item.SetTime,
            item.ValueDigits, item.MeasValue, unit, item.TimeDigits, item.MeasTime,
            item.Passed ? "passed" : "FAILED");
    }
}
