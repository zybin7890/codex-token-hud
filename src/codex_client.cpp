#include "codex_client.hpp"
#include <filesystem>
#include <vector>
#include <cwctype>
#include <cstring>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <regex>

namespace {
void release(HANDLE& handle) {
    if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    handle = nullptr;
}
std::wstring environment(const wchar_t* name) {
    DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (!length) return {};
    std::wstring value(length, L'\0');
    DWORD written = GetEnvironmentVariableW(name, value.data(), length);
    value.resize(written < length ? written : 0);
    return value;
}
bool is_file(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}
std::wstring search(const wchar_t* name) {
    std::vector<wchar_t> buffer(32768);
    DWORD length = SearchPathW(nullptr, name, nullptr, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    return length && length < buffer.size() ? std::wstring(buffer.data(), length) : std::wstring{};
}
std::wstring find_codex() {
    std::vector<std::filesystem::path> roots;
    auto shim = search(L"codex.cmd");
    if (!shim.empty()) roots.emplace_back(std::filesystem::path(shim).parent_path());
    auto appdata = environment(L"APPDATA");
    if (!appdata.empty()) roots.emplace_back(std::filesystem::path(appdata) / L"npm");
#if defined(_M_ARM64) || defined(__aarch64__)
    const auto package = L"codex-win32-arm64";
    const auto target = L"aarch64-pc-windows-msvc";
#else
    const auto package = L"codex-win32-x64";
    const auto target = L"x86_64-pc-windows-msvc";
#endif
    for (const auto& root : roots) {
        auto openai = root / L"node_modules" / L"@openai";
        for (const auto& vendor : {openai / L"codex" / L"node_modules" / L"@openai" / package / L"vendor",
                                   openai / package / L"vendor", openai / L"codex" / L"vendor"}) {
            for (const auto& folder : {L"bin", L"codex"}) {
                auto path = vendor / target / folder / L"codex.exe";
                if (is_file(path)) return path.wstring();
            }
        }
    }
    // Follow the npm CLI shim before a different, older codex.exe in PATH.
    return search(L"codex.exe");
}
}

std::wstring utf8_to_wide(const char* text) {
    if (!text || !*text) return {};
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (!length) return L"?";
    std::wstring output(length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, output.data(), length);
    output.pop_back();
    return output;
}

CodexClient::CodexClient(std::wstring executable) : executable_(std::move(executable)) {}
CodexClient::~CodexClient() { close(); }

void CodexClient::close() {
    release(input_); // EOF lets app-server shut down normally.
    if (process_ && WaitForSingleObject(process_, 800) == WAIT_TIMEOUT) {
        // The private job contains only the server launched by this monitor.
        if (job_) TerminateJobObject(job_, 0);
        else TerminateProcess(process_, 0);
        WaitForSingleObject(process_, 800);
    }
    release(output_);
    release(process_);
    release(job_);
    pending_.clear();
}

bool CodexClient::write(const char* message) {
    size_t length = strlen(message), offset = 0;
    while (offset < length) {
        DWORD written = 0;
        if (!WriteFile(input_, message + offset, static_cast<DWORD>(length - offset), &written, nullptr) || !written) return false;
        offset += written;
    }
    return true;
}

bool CodexClient::response(int id, std::string& message, const std::atomic_bool& stop) {
    const auto deadline = GetTickCount64() + 20000;
    while (!stop && GetTickCount64() < deadline) {
        size_t newline;
        while ((newline = pending_.find('\n')) != std::string::npos) {
            std::string line = pending_.substr(0, newline);
            pending_.erase(0, newline + 1);
            int64_t received = 0;
            if (pulse_response_id(line.data(), line.size(), &received) && received == id) {
                message = std::move(line);
                return true;
            }
        }
        DWORD available = 0;
        if (!PeekNamedPipe(output_, nullptr, 0, nullptr, &available, nullptr)) return false;
        if (available) {
            char buffer[8192];
            DWORD count = 0;
            if (!ReadFile(output_, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr)) return false;
            pending_.append(buffer, count);
            if (pending_.size() > 1024 * 1024) return false;
        } else {
            if (WaitForSingleObject(process_, 0) != WAIT_TIMEOUT) return false;
            Sleep(10);
        }
    }
    return false;
}

bool CodexClient::start(const std::atomic_bool& stop, std::wstring& error) {
    close();
    auto path = executable_.empty() ? find_codex() : executable_;
    std::filesystem::path file(path);
    auto extension = file.extension().wstring();
    for (auto& ch : extension) ch = std::towlower(ch);
    if (path.empty() || !is_file(file) || extension != L".exe" || path.find(L'"') != std::wstring::npos) {
        error = L"未找到 Codex CLI。请安装并登录 CLI，或用 --codex 指定 codex.exe。";
        return false;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE child_input = nullptr, child_output = nullptr, null_error = nullptr;
    if (!CreatePipe(&child_input, &input_, &sa, 0) || !CreatePipe(&output_, &child_output, &sa, 0)) {
        release(child_input); release(child_output); close();
        error = L"无法创建 Codex 通信管道。";
        return false;
    }
    SetHandleInformation(input_, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(output_, HANDLE_FLAG_INHERIT, 0);
    null_error = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job_ || !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        release(child_input); release(child_output); release(null_error); close();
        error = L"无法创建安全的 Codex 子进程。";
        return false;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = child_input;
    startup.hStdOutput = child_output;
    startup.hStdError = null_error;
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + path + L"\" app-server --stdio -c analytics.enabled=false";
    // Inherit CODEX_HOME and the CLI's existing login; no credential file is read here.
    BOOL created = CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process);
    release(child_input); release(child_output); release(null_error);
    if (!created) {
        close(); error = L"无法启动 Codex CLI。"; return false;
    }
    process_ = process.hProcess;
    if (!AssignProcessToJobObject(job_, process_)) {
        TerminateProcess(process_, 0);
        CloseHandle(process.hThread);
        close(); error = L"无法管理 Codex 子进程生命周期。"; return false;
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    std::string reply;
    if (!write("{\"id\":1,\"method\":\"initialize\",\"params\":{\"clientInfo\":{\"name\":\"codex_pulse\",\"version\":\"0.1.0\"},\"capabilities\":{\"experimentalApi\":false}}}\n") ||
        !response(1, reply, stop) || !pulse_response_ok(reply.data(), reply.size()) ||
        !write("{\"method\":\"initialized\"}\n")) {
        close(); error = L"Codex 接口初始化失败或超时，请确认 CLI 支持 app-server。"; return false;
    }
    return true;
}

FetchResult CodexClient::fetch(const std::atomic_bool& stop) {
    FetchResult result;
    if ((!process_ || WaitForSingleObject(process_, 0) != WAIT_TIMEOUT) && !start(stop, result.error)) return result;
    std::string reply;
    if (!write("{\"id\":2,\"method\":\"account/rateLimits/read\",\"params\":null}\n") || !response(2, reply, stop)) {
        close(); result.error = L"额度查询超时或连接中断，请检查网络后刷新。"; return result;
    }
    char error[256]{};
    if (!pulse_parse_response(reply.data(), reply.size(), &result.snapshot, error, sizeof(error))) {
        result.error = L"无法读取额度："+utf8_to_wide(error)+L"。请确认 CLI 已登录 ChatGPT。";
        return result;
    }
    result.ok = true;
    auto document=nlohmann::json::parse(reply,nullptr,false);
    if(!document.is_discarded()) {
        auto& body=document["result"];
        if(body.contains("rateLimitResetCredits") && body["rateLimitResetCredits"].is_object()) {
            auto count=body["rateLimitResetCredits"].value("availableCount",int64_t(-1));
            if(count>=0 && count<=INT_MAX)result.reset_credit_count=static_cast<int>(count);
        }
    }
    return result;
}

bool reset_allowed(const FetchResult& result) {
    if(!result.ok || result.reset_credit_count<1)return false;
    for(size_t i=0;i<result.snapshot.pool_count;++i) {
        const auto& pool=result.snapshot.pools[i];
        if(strcmp(pool.id,"codex") && !(result.snapshot.pool_count==1 && !*pool.id))continue;
        for(const auto* window:{&pool.primary,&pool.secondary}) {
            double remaining=pulse_remaining(window);
            if(remaining>=0 && remaining<=1.0)return true;
        }
    }
    return false;
}

std::string CodexClient::reset(const std::string& key,const std::atomic_bool& stop) {
    static const std::regex uuid("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$");
    if(!std::regex_match(key,uuid))return "invalidRequest";
    // A fresh server read enforces the user's <=1% rule, regardless of UI state.
    if(!reset_allowed(fetch(stop)))return "thresholdNotMet";
    nlohmann::json request{{"id",3},{"method","account/rateLimitResetCredit/consume"},{"params",{{"idempotencyKey",key}}}};
    std::string message=request.dump()+"\n",reply;
    if(!write(message.c_str()) || !response(3,reply,stop)){close();return "unknown";}
    auto document=nlohmann::json::parse(reply,nullptr,false);
    if(document.is_discarded() || !document.contains("result") || document.contains("error"))return "failed";
    std::string outcome=document["result"].value("outcome",std::string("unknown"));
    for(const auto* known:{"reset","alreadyRedeemed","noCredit","nothingToReset"})if(outcome==known)return outcome;
    return "unknown";
}
