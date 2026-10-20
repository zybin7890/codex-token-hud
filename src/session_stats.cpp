#include "session_stats.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>
#include <utility>

namespace {
constexpr std::uintmax_t read_limit = 2 * 1024 * 1024;
constexpr std::size_t line_limit = 1024 * 1024;
constexpr std::size_t session_limit = 32;
constexpr std::size_t day_limit = 7;

double event_seconds(const char* value) {
    int y=0,m=0,d=0,h=0,min=0; double sec=0;
    auto length=std::strlen(value);
    if(length<20 || value[length-1]!='Z' || std::sscanf(value,"%d-%d-%dT%d:%d:%lf",&y,&m,&d,&h,&min,&sec)!=6)return -1;
    const std::chrono::year_month_day date{std::chrono::year(y),std::chrono::month(m),std::chrono::day(d)};
    if(!date.ok() || h<0 || h>23 || min<0 || min>59 || sec<0 || sec>=60)return -1;
    return std::chrono::duration<double>(std::chrono::sys_days(date).time_since_epoch()).count()+h*3600+min*60+sec;
}

struct DirectoryNumber {
    std::filesystem::path path;
    unsigned value;
};

std::vector<DirectoryNumber> numbered_directories(const std::filesystem::path& root,
                                                std::size_t digits) {
    std::vector<DirectoryNumber> result;
    std::error_code error;
    for (std::filesystem::directory_iterator it(root, error), end;
         !error && it != end; it.increment(error)) {
        if (!it->is_directory(error) || error) continue;
        const auto name = it->path().filename().wstring();
        if (name.size() != digits) continue;
        unsigned value = 0;
        bool valid = true;
        for (const auto character : name) {
            if (character < L'0' || character > L'9') { valid = false; break; }
            value = value * 10 + static_cast<unsigned>(character - L'0');
        }
        if (valid) result.push_back({it->path(), value});
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.value > right.value;
    });
    return result;
}

std::filesystem::path session_home(const std::wstring& explicit_home) {
    if (!explicit_home.empty()) return explicit_home;
    if (const auto* value = _wgetenv(L"CODEX_HOME"); value && *value) return value;
    if (const auto* value = _wgetenv(L"USERPROFILE"); value && *value)
        return std::filesystem::path(value) / L".codex";
    return {};
}

bool rollout_file(const std::filesystem::path& path) {
    const auto name = path.filename().wstring();
    return name.starts_with(L"rollout-") && path.extension() == L".jsonl";
}
}

std::vector<SessionEntry> list_sessions(const std::wstring& home) {
    const auto base = session_home(home);
    if (base.empty()) return {};
    struct Candidate {
        std::filesystem::path path;
        std::filesystem::file_time_type modified;
    };
    std::vector<Candidate> candidates;
    std::size_t days = 0;
    for (const auto& year : numbered_directories(base / L"sessions", 4)) {
        for (const auto& month : numbered_directories(year.path, 2)) {
            for (const auto& day : numbered_directories(month.path, 2)) {
                const std::chrono::year_month_day date{std::chrono::year(static_cast<int>(year.value)),
                    std::chrono::month(month.value), std::chrono::day(day.value)};
                if (!date.ok()) continue;
                bool found = false;
                std::error_code error;
                for (std::filesystem::directory_iterator it(day.path, error), end;
                     !error && it != end; it.increment(error)) {
                    if (!it->is_regular_file(error) || error || !rollout_file(it->path())) continue;
                    const auto modified = it->last_write_time(error);
                    if (error) continue;
                    found = true;
                    candidates.push_back({it->path(), modified});
                }
                if (found && ++days == day_limit) break;
            }
            if (days == day_limit) break;
        }
        if (days == day_limit) break;
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        if (left.modified != right.modified) return left.modified > right.modified;
        return left.path.native() < right.path.native();
    });
    if (candidates.size() > session_limit) candidates.resize(session_limit);
    std::vector<SessionEntry> result;
    result.reserve(candidates.size());
    for (auto& candidate : candidates) {
        auto label = candidate.path.stem().wstring();
        label.erase(0, std::wstring(L"rollout-").size());
        result.push_back({std::move(candidate.path), std::move(label)});
    }
    return result;
}

void SessionReader::reset(const std::filesystem::path& path) {
    path_ = path;
    offset_ = 0;
    initialized_ = false;
    discard_line_ = false;
    pending_.clear();
    stats_ = {};
    stats_.usage.model_context_window = -1;
}

void SessionReader::accept_line() {
    if (!pending_.empty() && pending_.back() == '\r') pending_.pop_back();
    if (pending_.empty()) return;
    PulseUsage parsed{};
    parsed.model_context_window = -1;
    const int status = pulse_parse_usage_event(pending_.data(), pending_.size(), &parsed);
    if (status == 1) {
        const auto& previous=stats_.usage;
        const double before=event_seconds(previous.timestamp), after=event_seconds(parsed.timestamp);
        const double elapsed=after-before;
        if(previous.total.present && parsed.total.present) {
            if(parsed.total.output_tokens<previous.total.output_tokens)stats_.output_tokens_per_second=-1;
            else if(parsed.total.output_tokens>previous.total.output_tokens && before>=0 && elapsed>0 && elapsed<=120)
                stats_.output_tokens_per_second=(parsed.total.output_tokens-previous.total.output_tokens)/elapsed;
        }
        stats_.usage = parsed;
        stats_.error.clear();
    } else if (status < 0) {
        stats_.error = L"日志行无效；统计保留在上一次完整快照。";
    }
}

void SessionReader::consume(const char* bytes, std::size_t length) {
    for (std::size_t index = 0; index < length; ++index) {
        const char character = bytes[index];
        if (character == '\n') {
            if (!discard_line_) accept_line();
            pending_.clear();
            discard_line_ = false;
        } else if (!discard_line_) {
            if (pending_.size() == line_limit) {
                pending_.clear();
                discard_line_ = true;
                stats_.tail_limited = true;
                stats_.error = L"已跳过超过 1 MiB 的日志行；统计保留在上一次完整快照。";
            } else {
                pending_.push_back(character);
            }
        }
    }
}

SessionStats SessionReader::read(const std::filesystem::path& path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error) absolute = path;
    absolute = absolute.lexically_normal();
    if (path_ != absolute) reset(absolute);
    const auto size = std::filesystem::file_size(absolute, error);
    if (error) {
        stats_.error = L"无法读取所选会话日志。";
        return stats_;
    }
    if (initialized_ && size < offset_) reset(absolute);
    bool check_boundary = false;
    if (!initialized_) {
        // Reserve one byte of the initial read budget to check whether the
        // chosen tail begins at a complete line boundary.
        offset_ = size > read_limit ? size - (read_limit - 1) : 0;
        discard_line_ = offset_ != 0;
        check_boundary = discard_line_;
        stats_.tail_limited = discard_line_;
        initialized_ = true;
    }
    if (size == offset_) return stats_;
    std::ifstream stream(absolute, std::ios::binary);
    if (!stream) {
        stats_.error = L"无法打开所选会话日志。";
        return stats_;
    }
    if (check_boundary) {
        stream.seekg(static_cast<std::streamoff>(offset_ - 1));
        char preceding = 0;
        if (!stream.get(preceding)) {
            stats_.error = L"无法定位所选会话日志。";
            return stats_;
        }
        discard_line_ = preceding != '\n';
    }
    stream.seekg(static_cast<std::streamoff>(offset_));
    if (!stream) {
        stats_.error = L"无法定位所选会话日志。";
        return stats_;
    }
    const auto budget = check_boundary ? read_limit - 1 : read_limit;
    auto remaining = std::min(size - offset_, budget);
    if (size - offset_ > read_limit) stats_.tail_limited = true;
    std::array<char, 64 * 1024> buffer{};
    while (remaining != 0) {
        const auto requested = static_cast<std::streamsize>(std::min<std::uintmax_t>(remaining, buffer.size()));
        stream.read(buffer.data(), requested);
        const auto received = stream.gcount();
        if (received <= 0) {
            stats_.error = L"日志在读取时发生变化；统计保留在上一次完整快照。";
            break;
        }
        consume(buffer.data(), static_cast<std::size_t>(received));
        offset_ += static_cast<std::uintmax_t>(received);
        remaining -= static_cast<std::uintmax_t>(received);
    }
    return stats_;
}
