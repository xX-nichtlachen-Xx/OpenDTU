#pragma once
#include "Parser.h"
#include "types.h"
#include <array>
#include <vector>

#define SELF_CHECK_SIZE 128
#define SELF_CHECK_HEADER_SIZE 8
#define SELF_CHECK_ITEM_SIZE 10
#define SELF_CHECK_ITEM_COUNT 8
#define SELF_CHECK_FRAGMENT_COUNT 13

#define HOY_SELF_CHECK_POLL_INTERVAL 1000
#define HOY_SELF_CHECK_TIMEOUT (130 * 1000)
#define HOY_SELF_CHECK_COLLECT_POLLS 60

enum class SelfCheckState : uint8_t {
    Idle,
    Starting,
    Running,
    Finished,
    Timeout,
    StartFailed,
    Unsupported,
    Reset,
    Collecting,
    Incomplete,
};

struct SelfCheckItem_t {
    bool Present;
    bool Passed;
    uint16_t Code;
    uint8_t ValueDigits;
    uint8_t TimeDigits;
    float SetValue;
    float SetTime;
    float MeasValue;
    float MeasTime;
};

class SelfCheckParser : public Parser {
public:
    SelfCheckParser();
    void clearBuffer();
    void appendFragment(const uint8_t offset, const uint8_t* payload, const uint8_t len);

    void evaluateBuffer();
    bool addPartialFragments(const fragment_t fragment[], const uint8_t last_fragment_id, const uint8_t max_fragment_id);

    void setStarting();
    void setStartResult(const bool success);
    void setTimeout();
    void setUnsupported();
    void setIncomplete();

    SelfCheckState getState() const;
    bool isActive() const;
    uint32_t getStartTime() const;
    uint32_t getPollCount() const;
    uint32_t getLastPoll() const;
    void setLastPoll(const uint32_t lastPoll);

    uint8_t getIpsState() const;
    uint16_t getProfileId() const;
    String getProfileName() const;
    String getProfileVersion() const;

    uint8_t getItemCount() const;
    SelfCheckItem_t getItem(const uint8_t idx) const;

    std::vector<uint8_t> getRawData() const;

    uint32_t getCollectPolls() const;
    std::vector<uint8_t> getReceivedFragments() const;
    uint8_t getFragmentTotal() const;

    static const char* getIpsStateName(const uint8_t ipsState);
    static const char* getStateName(const SelfCheckState state);

    static const char* getItemId(const uint8_t idx);
    static const char* getItemUnit(const uint8_t idx);

private:
    void clearFragmentCache();
    bool assembleFragmentCache();

    uint8_t _payloadSelfCheck[SELF_CHECK_SIZE] = {};
    uint8_t _selfCheckLength = 0;

    fragment_t _fragmentCache[SELF_CHECK_FRAGMENT_COUNT] = {};
    uint8_t _fragmentCacheMaxId = 0;

    SelfCheckState _state = SelfCheckState::Idle;
    uint32_t _startTime = 0;
    uint32_t _pollCount = 0;
    uint32_t _lastPoll = 0;
    uint32_t _collectPolls = 0;
    bool _seenRunning = false;
};
