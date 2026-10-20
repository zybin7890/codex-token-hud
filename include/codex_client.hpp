#pragma once
#include <windows.h>
#include <atomic>
#include <string>
#include "pulse_quota.h"

struct FetchResult {
    PulseSnapshot snapshot{};
    std::wstring error;
    bool ok = false;
    int reset_credit_count = -1;
};

// Only initializes a local CLI server and reads account quotas. Never starts a turn.
class CodexClient {
public:
    explicit CodexClient(std::wstring executable = {});
    ~CodexClient();
    CodexClient(const CodexClient&) = delete;
    CodexClient& operator=(const CodexClient&) = delete;
    FetchResult fetch(const std::atomic_bool& stop);
    std::string reset(const std::string& idempotency_key, const std::atomic_bool& stop);
    void close();
private:
    bool start(const std::atomic_bool& stop, std::wstring& error);
    bool write(const char* message);
    bool response(int id, std::string& message, const std::atomic_bool& stop);
    std::wstring executable_;
    HANDLE process_ = nullptr, input_ = nullptr, output_ = nullptr, job_ = nullptr;
    std::string pending_;
};

std::wstring utf8_to_wide(const char* text);
bool reset_allowed(const FetchResult& result);
