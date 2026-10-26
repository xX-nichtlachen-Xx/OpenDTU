#include "SelfCheckParser.h"
#include "GridProfileParser.h"
#include "crc.h"
#include <cstring>
#include <esp_log.h>

#undef TAG
static const char* TAG = "hoymiles";

static constexpr std::array<const char*, SELF_CHECK_ITEM_COUNT> itemIds = {
    "ov1", "uv1", "ov2", "uv2", "of1", "uf1", "of2", "uf2"
};

static constexpr std::array<const char*, SELF_CHECK_ITEM_COUNT> itemUnits = {
    "V", "V", "V", "V", "Hz", "Hz", "Hz", "Hz"
};

static float powerOfTen(const uint8_t exp)
{
    float ret = 1;
    for (uint8_t i = 0; i < exp; i++) {
        ret *= 10;
    }
    return ret;
}

SelfCheckParser::SelfCheckParser()
    : Parser()
{
    clearBuffer();
}

void SelfCheckParser::clearBuffer()
{
    memset(_payloadSelfCheck, 0, SELF_CHECK_SIZE);
    _selfCheckLength = 0;
}

void SelfCheckParser::appendFragment(const uint8_t offset, const uint8_t* payload, const uint8_t len)
{
    if (offset + len > SELF_CHECK_SIZE) {
        ESP_LOGE(TAG, "(%s, %d) self-check packet too large for buffer", __FILE__, __LINE__);
        return;
    }
    memcpy(&_payloadSelfCheck[offset], payload, len);
    _selfCheckLength += len;
}

void SelfCheckParser::evaluateBuffer()
{
    HOY_SEMAPHORE_TAKE();
    _pollCount++;
    if (_state == SelfCheckState::Running || _state == SelfCheckState::Collecting) {
        if (_selfCheckLength >= SELF_CHECK_HEADER_SIZE) {
            const uint8_t ipsState = _payloadSelfCheck[3];
            if (ipsState >= 2) {
                _state = SelfCheckState::Finished;
                clearFragmentCache();
            } else if (ipsState == 1) {
                _seenRunning = true;
            } else if (_seenRunning) {
                _state = SelfCheckState::Reset;
            }
        } else if (_selfCheckLength >= 4 && _payloadSelfCheck[0] == 0x1E && _payloadSelfCheck[1] == 0x00) {
            _state = SelfCheckState::Unsupported;
        }
    }
    HOY_SEMAPHORE_GIVE();
}

bool SelfCheckParser::addPartialFragments(const fragment_t fragment[], const uint8_t last_fragment_id, const uint8_t max_fragment_id)
{
    bool complete = false;

    HOY_SEMAPHORE_TAKE();
    if (_state == SelfCheckState::Running || _state == SelfCheckState::Collecting) {
        const uint8_t count = last_fragment_id < SELF_CHECK_FRAGMENT_COUNT ? last_fragment_id : SELF_CHECK_FRAGMENT_COUNT;

        for (uint8_t i = 0; i < count; i++) {
            const fragment_t& cached = _fragmentCache[i];
            if (fragment[i].wasReceived && fragment[i].mainCmd == 0x95 && cached.wasReceived
                && (cached.len != fragment[i].len || memcmp(cached.fragment, fragment[i].fragment, fragment[i].len) != 0)) {
                clearFragmentCache();
                break;
            }
        }

        bool stored = false;
        for (uint8_t i = 0; i < count; i++) {
            if (fragment[i].wasReceived && fragment[i].mainCmd == 0x95) {
                _fragmentCache[i] = fragment[i];
                stored = true;
            }
        }

        if (stored) {
            if (max_fragment_id > 0) {
                _fragmentCacheMaxId = max_fragment_id;
            }
            _state = SelfCheckState::Collecting;
            complete = assembleFragmentCache();
        }
    }
    HOY_SEMAPHORE_GIVE();

    return complete;
}

void SelfCheckParser::clearFragmentCache()
{
    memset(_fragmentCache, 0, sizeof(_fragmentCache));
    _fragmentCacheMaxId = 0;
}

bool SelfCheckParser::assembleFragmentCache()
{
    if (_fragmentCacheMaxId == 0) {
        return false;
    }

    for (uint8_t i = 0; i < _fragmentCacheMaxId; i++) {
        if (!_fragmentCache[i].wasReceived) {
            return false;
        }
    }

    uint8_t buffer[SELF_CHECK_SIZE];
    uint8_t len = 0;
    for (uint8_t i = 0; i < _fragmentCacheMaxId; i++) {
        if (len + _fragmentCache[i].len > SELF_CHECK_SIZE) {
            clearFragmentCache();
            return false;
        }
        memcpy(&buffer[len], _fragmentCache[i].fragment, _fragmentCache[i].len);
        len += _fragmentCache[i].len;
    }

    if (len < SELF_CHECK_HEADER_SIZE + 2
        || crc16(buffer, len - 2) != ((buffer[len - 2] << 8) | buffer[len - 1])) {
        ESP_LOGW(TAG, "self-check fragments from different answers, CRC mismatch");
        clearFragmentCache();
        return false;
    }

    memcpy(_payloadSelfCheck, buffer, len);
    _selfCheckLength = len;
    _pollCount++;
    _state = SelfCheckState::Finished;
    clearFragmentCache();
    return true;
}

void SelfCheckParser::setStarting()
{
    HOY_SEMAPHORE_TAKE();
    clearBuffer();
    clearFragmentCache();
    _state = SelfCheckState::Starting;
    _startTime = millis();
    _pollCount = 0;
    _lastPoll = 0;
    _collectPolls = 0;
    _seenRunning = false;
    HOY_SEMAPHORE_GIVE();
}

void SelfCheckParser::setStartResult(const bool success)
{
    HOY_SEMAPHORE_TAKE();
    if (_state == SelfCheckState::Starting) {
        _state = success ? SelfCheckState::Running : SelfCheckState::StartFailed;
        _startTime = millis();
    }
    HOY_SEMAPHORE_GIVE();
}

void SelfCheckParser::setTimeout()
{
    HOY_SEMAPHORE_TAKE();
    _state = SelfCheckState::Timeout;
    HOY_SEMAPHORE_GIVE();
}

void SelfCheckParser::setUnsupported()
{
    HOY_SEMAPHORE_TAKE();
    _state = SelfCheckState::Unsupported;
    HOY_SEMAPHORE_GIVE();
}

void SelfCheckParser::setIncomplete()
{
    HOY_SEMAPHORE_TAKE();
    _state = SelfCheckState::Incomplete;
    HOY_SEMAPHORE_GIVE();
}

SelfCheckState SelfCheckParser::getState() const
{
    return _state;
}

bool SelfCheckParser::isActive() const
{
    return _state == SelfCheckState::Starting || _state == SelfCheckState::Running || _state == SelfCheckState::Collecting;
}

uint32_t SelfCheckParser::getStartTime() const
{
    return _startTime;
}

uint32_t SelfCheckParser::getPollCount() const
{
    return _pollCount;
}

uint32_t SelfCheckParser::getLastPoll() const
{
    return _lastPoll;
}

void SelfCheckParser::setLastPoll(const uint32_t lastPoll)
{
    _lastPoll = lastPoll;
    if (_state == SelfCheckState::Collecting) {
        _collectPolls++;
    }
}

uint32_t SelfCheckParser::getCollectPolls() const
{
    return _collectPolls;
}

std::vector<uint8_t> SelfCheckParser::getReceivedFragments() const
{
    std::vector<uint8_t> ret;
    HOY_SEMAPHORE_TAKE();
    for (uint8_t i = 0; i < SELF_CHECK_FRAGMENT_COUNT; i++) {
        if (_fragmentCache[i].wasReceived) {
            ret.push_back(i + 1);
        }
    }
    HOY_SEMAPHORE_GIVE();
    return ret;
}

uint8_t SelfCheckParser::getFragmentTotal() const
{
    return _fragmentCacheMaxId;
}

uint8_t SelfCheckParser::getIpsState() const
{
    if (_selfCheckLength < SELF_CHECK_HEADER_SIZE) {
        return 0;
    }
    return _payloadSelfCheck[3];
}

uint16_t SelfCheckParser::getProfileId() const
{
    return (_payloadSelfCheck[4] << 8) | _payloadSelfCheck[5];
}

String SelfCheckParser::getProfileName() const
{
    if (_selfCheckLength < SELF_CHECK_HEADER_SIZE) {
        return "";
    }
    return GridProfileParser::getProfileNameById(_payloadSelfCheck[4], _payloadSelfCheck[5]);
}

String SelfCheckParser::getProfileVersion() const
{
    if (_selfCheckLength < SELF_CHECK_HEADER_SIZE) {
        return "";
    }
    char buffer[10];
    HOY_SEMAPHORE_TAKE();
    snprintf(buffer, sizeof(buffer), "%d.%d.%d", (_payloadSelfCheck[6] >> 4) & 0x0f, _payloadSelfCheck[6] & 0x0f, _payloadSelfCheck[7]);
    HOY_SEMAPHORE_GIVE();
    return buffer;
}

uint8_t SelfCheckParser::getItemCount() const
{
    if (_selfCheckLength < SELF_CHECK_HEADER_SIZE + SELF_CHECK_ITEM_SIZE + 2) {
        return 0;
    }
    const uint8_t count = (_selfCheckLength - 2 - SELF_CHECK_HEADER_SIZE) / SELF_CHECK_ITEM_SIZE;
    return count > SELF_CHECK_ITEM_COUNT ? SELF_CHECK_ITEM_COUNT : count;
}

SelfCheckItem_t SelfCheckParser::getItem(const uint8_t idx) const
{
    SelfCheckItem_t item = {};
    if (idx >= getItemCount()) {
        return item;
    }

    HOY_SEMAPHORE_TAKE();
    const uint8_t* p = &_payloadSelfCheck[SELF_CHECK_HEADER_SIZE + idx * SELF_CHECK_ITEM_SIZE];
    item.Present = true;
    item.Code = (p[0] << 8) | p[1];
    item.Passed = (item.Code & 0x0001) != 0;
    item.ValueDigits = (item.Code >> 12) & 0x0f;
    item.TimeDigits = (item.Code >> 8) & 0x0f;

    const float valueDiv = powerOfTen(item.ValueDigits);
    const float timeDiv = powerOfTen(item.TimeDigits);
    item.SetValue = static_cast<uint16_t>((p[2] << 8) | p[3]) / valueDiv;
    item.SetTime = static_cast<uint16_t>((p[4] << 8) | p[5]) / timeDiv;
    item.MeasValue = static_cast<uint16_t>((p[6] << 8) | p[7]) / valueDiv;
    item.MeasTime = static_cast<uint16_t>((p[8] << 8) | p[9]) / timeDiv;
    HOY_SEMAPHORE_GIVE();

    return item;
}

std::vector<uint8_t> SelfCheckParser::getRawData() const
{
    std::vector<uint8_t> ret;
    HOY_SEMAPHORE_TAKE();
    for (uint8_t i = 0; i < _selfCheckLength; i++) {
        ret.push_back(_payloadSelfCheck[i]);
    }
    HOY_SEMAPHORE_GIVE();
    return ret;
}

const char* SelfCheckParser::getIpsStateName(const uint8_t ipsState)
{
    switch (ipsState) {
    case 2:
        return "Success";
    case 3:
        return "Fail1";
    case 4:
        return "Fail2";
    case 5:
        return "Fail3";
    case 6:
        return "Fail4";
    default:
        return "N/A";
    }
}

const char* SelfCheckParser::getStateName(const SelfCheckState state)
{
    switch (state) {
    case SelfCheckState::Starting:
        return "starting";
    case SelfCheckState::Running:
        return "running";
    case SelfCheckState::Finished:
        return "finished";
    case SelfCheckState::Timeout:
        return "timeout";
    case SelfCheckState::StartFailed:
        return "start_failed";
    case SelfCheckState::Unsupported:
        return "unsupported";
    case SelfCheckState::Reset:
        return "reset";
    case SelfCheckState::Collecting:
        return "collecting";
    case SelfCheckState::Incomplete:
        return "incomplete";
    default:
        return "idle";
    }
}

const char* SelfCheckParser::getItemId(const uint8_t idx)
{
    return idx < SELF_CHECK_ITEM_COUNT ? itemIds[idx] : "";
}

const char* SelfCheckParser::getItemUnit(const uint8_t idx)
{
    return idx < SELF_CHECK_ITEM_COUNT ? itemUnits[idx] : "";
}
