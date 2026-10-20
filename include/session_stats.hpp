#pragma once

#include "pulse_quota.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct SessionEntry {
    std::filesystem::path path;
    std::wstring label;
};

struct SessionStats {
    PulseUsage usage{};
    double output_tokens_per_second = -1;
    bool tail_limited = false;
    std::wstring error;
};

// Recent log files, ordered by modification time; this does not identify the
// session shown in the foreground Codex window.
std::vector<SessionEntry> list_sessions(const std::wstring& home = {});

class SessionReader {
public:
    SessionStats read(const std::filesystem::path& path);

private:
    void reset(const std::filesystem::path& path);
    void accept_line();
    void consume(const char* bytes, std::size_t length);

    std::filesystem::path path_;
    std::uintmax_t offset_ = 0;
    bool initialized_ = false;
    bool discard_line_ = false;
    std::string pending_;
    SessionStats stats_;
};
