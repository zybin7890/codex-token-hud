#include "desktop_launch.hpp"
#include "cdp_client.hpp"
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <appmodel.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
};

std::filesystem::path executable_dir() {
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size()) throw std::runtime_error("Launcher path unavailable.");
    path.resize(size);
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path profile_dir() {
    const DWORD size = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
    if (!size) throw std::runtime_error("APPDATA unavailable.");
    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(L"APPDATA", value.data(), size);
    if (!written || written >= size) throw std::runtime_error("APPDATA unavailable.");
    value.resize(written);
    return std::filesystem::path(value) / L"Codex" / L"web" / L"Codex";
}

uint16_t ready_port(const std::filesystem::path& profile) {
    std::ifstream file(profile / L"DevToolsActivePort");
    std::string line;
    if (!std::getline(file, line) || line.size() > 6) return 0;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    unsigned port = 0;
    for (const char digit : line) {
        if (digit < '0' || digit > '9') return 0;
        port = port * 10 + digit - '0';
    }
    if (!port || port > 65535) return 0;
    try {
        for (const auto& page : cdp_targets(static_cast<uint16_t>(port))) {
            if (page.value("type", "") != "page") continue;
            const auto url = page.value("url", "");
            for (const auto* base : {"app://-/index.html", "app://-/detached-window.html"}) {
                const std::string prefix = base;
                if (url == prefix || url.starts_with(prefix + "?") || url.starts_with(prefix + "#"))
                    return static_cast<uint16_t>(port);
            }
        }
    } catch (const std::exception&) {}
    return 0;
}

DWORD desktop_for_port(uint16_t port) {
    DWORD size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0)
        != ERROR_INSUFFICIENT_BUFFER) return 0;
    std::vector<unsigned char> buffer(size);
    if (GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET,
                            TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) return 0;
    const auto table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
    for (DWORD index = 0; index < table->dwNumEntries; ++index) {
        const auto& row = table->table[index];
        if (ntohs(static_cast<u_short>(row.dwLocalPort)) != port ||
            row.dwLocalAddr != htonl(INADDR_LOOPBACK)) continue;
        Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, row.dwOwningPid)};
        if (!process.value) continue;
        wchar_t family[256]{};
        UINT32 length = 256;
        if (GetPackageFamilyName(process.value, &length, family) == ERROR_SUCCESS &&
            std::wstring(family) == L"OpenAI.Codex_2p2nqsd0c76g0") return row.dwOwningPid;
    }
    return 0;
}

void start_collector(const std::filesystem::path& directory,
                     const std::filesystem::path& profile, uint16_t port, DWORD desktop_pid) {
    const auto executable = directory / L"codex-dashboard.exe";
    std::wstring command = L"\"" + executable.wstring() + L"\" --attach --profile \"" +
        profile.wstring() + L"\" --cdp-port " + std::to_wstring(port) +
        L" --desktop-pid " + std::to_wstring(desktop_pid);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process))
        throw std::runtime_error("Collector startup failed.");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    Handle lock{CreateMutexW(nullptr, FALSE, L"Local\\CodexTokenHudLauncher")};
    if (!lock.value) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    try {
        const auto directory = executable_dir();
        if (!std::filesystem::is_regular_file(directory / L"codex-dashboard.exe") ||
            !std::filesystem::is_regular_file(directory / L"widget" / L"inline.js"))
            throw std::runtime_error("Installation incomplete.");
        const auto profile = profile_dir();
        launch_codex_desktop();
        const ULONGLONG deadline = GetTickCount64() + 25000;
        do {
            if (const auto port = ready_port(profile)) {
                // Existing-instance activation can return a short-lived forwarding
                // process. Follow the verified package owning the loopback listener.
                if (const auto pid = desktop_for_port(port)) {
                    start_collector(directory, profile, port, pid);
                    return 0;
                }
            }
            Sleep(400);
        } while (GetTickCount64() < deadline);
        MessageBoxW(nullptr,
            L"Codex 已打开，但当前进程没有本地调试入口。\n"
            L"请先完成活动聊天并正常退出 Codex，再从桌面或开始菜单的 Codex 入口打开。\n"
            L"此入口不会结束或重启正在运行的聊天。",
            L"Codex 状态栏", MB_OK | MB_ICONINFORMATION);
        return 4;
    } catch (const std::exception&) {
        MessageBoxW(nullptr, L"Codex 状态栏启动失败。请核对 Codex 安装及状态栏程序文件。",
                    L"Codex 状态栏", MB_OK | MB_ICONWARNING);
        return 1;
    }
}
