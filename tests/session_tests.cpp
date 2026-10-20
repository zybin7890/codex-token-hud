#include "session_stats.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string usage(std::uint64_t input, std::int64_t context = 475000, std::uint64_t output = 2, const std::string& timestamp = "2026-10-13T13:00:00Z") {
    const auto counts = "{\"input_tokens\":" + std::to_string(input) +
        ",\"cached_input_tokens\":5,\"cache_write_input_tokens\":0,\"output_tokens\":" + std::to_string(output) +
        ",\"reasoning_output_tokens\":1,\"total_tokens\":" + std::to_string(input + output) + "}";
    return "{\"timestamp\":\"" + timestamp + "\",\"type\":\"event_msg\",\"payload\":{"
        "\"type\":\"token_count\",\"info\":{\"total_token_usage\":" + counts +
        ",\"last_token_usage\":" + counts + ",\"model_context_window\":" +
        (context < 0 ? "null" : std::to_string(context)) + "}}}";
}

void write(const std::filesystem::path& file, const std::string& bytes, bool append = false) {
    std::ofstream stream(file, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(stream), "fixture write failed");
}
}

int main(int argc, char** argv) {
    try {
        const auto base = argc > 1 ? std::filesystem::path(argv[1]) :
            std::filesystem::current_path() / "work" / "session-fixture";
        const auto run = base / ("run-" + std::to_string(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(run);
        const auto first = run / "first.jsonl", second = run / "second.jsonl";
        const auto speed_file = run / "speed.jsonl";
        SessionReader speed_reader;
        write(speed_file, usage(10,475000,2,"2026-10-13T13:00:00.000Z")+"\n"+usage(10,475000,102,"2026-10-13T13:00:02.500Z")+"\n");
        require(speed_reader.read(speed_file).output_tokens_per_second == 40, "event speed must use output delta and timestamp delta");
        require(speed_reader.read(speed_file).output_tokens_per_second == 40, "unchanged log must retain latest speed");
        write(speed_file, usage(10,475000,1,"2026-10-13T13:00:03.000Z")+"\n",true);
        require(speed_reader.read(speed_file).output_tokens_per_second < 0, "counter rollback must clear speed");
        require(speed_reader.read(second).output_tokens_per_second < 0, "switch must clear speed");
        SessionReader reader;
        write(first, usage(10) + "\n");
        auto result = reader.read(first);
        require(result.usage.total.present && result.usage.total.input_tokens == 10,
                "initial complete snapshot unavailable");
        require(result.usage.model_context_window == 475000, "context not parsed");
        write(first, usage(20) + "\n", true);
        result = reader.read(first);
        require(result.usage.total.input_tokens == 20, "append was summed or omitted");
        require(reader.read(first).usage.total.input_tokens == 20, "repeat read duplicated usage");

        const auto partial = usage(30);
        write(first, partial.substr(0, partial.size() / 2), true);
        require(reader.read(first).usage.total.input_tokens == 20, "partial line was accepted");
        write(first, partial.substr(partial.size() / 2), true);
        require(reader.read(first).usage.total.input_tokens == 20, "line without newline was accepted");
        write(first, "\r\n", true);
        require(reader.read(first).usage.total.input_tokens == 30, "partial append was lost");

        auto invalid = usage(30);
        invalid.replace(invalid.find("\"input_tokens\":30"), std::string("\"input_tokens\":30").size(),
                        "\"input_tokens\":-1");
        write(first, invalid + "\n", true);
        result = reader.read(first);
        require(result.usage.total.input_tokens == 30 && !result.error.empty(),
                "invalid latest usage must preserve old data and show error");
        require(!reader.read(first).error.empty(), "invalid latest error disappeared without new data");
        write(first, usage(40) + "\n", true);
        require(reader.read(first).error.empty(), "valid new usage did not clear stale error");

        write(second, "{\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":null}}\n");
        result = reader.read(second);
        require(!result.usage.total.present && result.usage.model_context_window == -1,
                "session switch leaked previous data");
        write(second, usage(50, -1) + "\n", true);
        result = reader.read(second);
        require(result.usage.total.input_tokens == 50 && result.usage.model_context_window == -1,
                "unknown context was not preserved");
        write(second, "{}\n");
        result = reader.read(second);
        require(!result.usage.total.present && result.usage.model_context_window == -1,
                "truncate did not reset old usage");
        write(second, usage(60) + "\n", true);
        require(reader.read(second).usage.total.input_tokens == 60, "post-truncate append failed");

        write(second, std::string(1024 * 1024 + 1, 'x') + "\n", true);
        result = reader.read(second);
        require(result.tail_limited && !result.error.empty() && result.usage.total.input_tokens == 60,
                "oversized row was not bounded and marked");
        write(second, usage(70) + "\n", true);
        result = reader.read(second);
        require(result.usage.total.input_tokens == 70 && result.error.empty(), "oversized-row recovery failed");

        const auto large = run / "large.jsonl";
        write(large, std::string(3 * 1024 * 1024, 'x') + "\n" + usage(80) + "\n");
        result = reader.read(large);
        require(result.tail_limited && result.usage.total.input_tokens == 80,
                "bounded initial tail did not skip incomplete opening row");
        std::string backlog;
        const std::string unrelated = "{\"type\":\"world_state\",\"payload\":{\"ignored\":\"" + std::string(100, 'x') + "\"}}\n";
        while (backlog.size() <= 2 * 1024 * 1024 + 1024) backlog += unrelated;
        backlog += usage(90) + "\n";
        write(large, backlog, true);
        result = reader.read(large);
        require(result.usage.total.input_tokens == 80 && result.tail_limited,
                "incremental read exceeded 2 MiB budget");
        require(reader.read(large).usage.total.input_tokens == 90, "bounded backlog continuation failed");

        const auto aligned = run / "aligned.jsonl";
        std::string suffix = usage(100) + "\n";
        while (suffix.size() + unrelated.size() <= 2 * 1024 * 1024 - 1) suffix += unrelated;
        suffix.append(2 * 1024 * 1024 - 1 - suffix.size(), ' ');
        write(aligned, std::string(4096, 'x') + "\n" + suffix);
        require(reader.read(aligned).usage.total.input_tokens == 100,
                "initial tail dropped a complete first line at newline boundary");

        const auto missing = run / "missing.jsonl";
        result = reader.read(missing);
        require(!result.usage.total.present && !result.error.empty(), "missing switched file leaked prior usage");

        const auto home = run / "home";
        const auto modification = std::filesystem::file_time_type::clock::now();
        for (unsigned day = 1; day <= 8; ++day) {
            const auto dir = home / "sessions" / "2026" / "10" / ("0" + std::to_string(day));
            std::filesystem::create_directories(dir);
            for (unsigned index = 0; index < 6; ++index) {
                const auto file = dir / ("rollout-2026-10-0" + std::to_string(day) +
                    "T12-00-00-" + std::to_string(index) + ".jsonl");
                write(file, "");
                std::filesystem::last_write_time(file, modification +
                    std::chrono::seconds(day * 100 + index));
            }
        }
        const auto sessions = list_sessions(home.wstring());
        require(sessions.size() == 32, "recent session list did not enforce 32-file cap");
        require(sessions.front().label.starts_with(L"2026-10-08"), "recent sessions not sorted by mtime");
        for (const auto& entry : sessions)
            require(entry.path.parent_path().filename() != L"01", "session list visited eighth recorded date");

        const auto sparse_home = run / "sparse-home";
        for (unsigned day = 1; day <= 8; ++day) {
            const auto dir = sparse_home / "sessions" / "2026" / "10" / ("0" + std::to_string(day));
            std::filesystem::create_directories(dir);
            write(dir / ("rollout-2026-10-0" + std::to_string(day) + "T12-00-00-fixture.jsonl"), "");
        }
        const auto sparse = list_sessions(sparse_home.wstring());
        require(sparse.size() == 7, "recent directory limit was hidden by file limit");
        for (const auto& entry : sparse)
            require(entry.path.parent_path().filename() != L"01", "session list included eighth recorded date");

        std::cout << "PASS: session append, partial lines, switch, truncation, bounds, errors, and recent selection\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }
}
