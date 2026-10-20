#include "cdp_client.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
constexpr std::size_t message_limit = 2 * 1024 * 1024;
constexpr auto request_timeout = std::chrono::seconds(5);

class InternetHandle {
public:
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET value) : value_(value) {}
    ~InternetHandle() { close(); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;

    HINTERNET get() const { return value_.load(); }
    HINTERNET release() { return value_.exchange(nullptr); }
    void set(HINTERNET value) { close(); value_.store(value); }
    void close() {
        if (const auto value = release()) WinHttpCloseHandle(value);
    }

private:
    std::atomic<HINTERNET> value_ = nullptr;
};

// A synchronous WinHTTP WebSocket receive can outlive inherited HTTP timeouts.
// Cancel only the owned handle at the request deadline; joining the watchdog
// before destroying the handle also prevents a late close of a later command.
class RequestDeadline {
public:
    explicit RequestDeadline(InternetHandle& handle)
        : handle_(handle), until_(std::chrono::steady_clock::now() + request_timeout),
          worker_([this] {
              std::unique_lock lock(mutex_);
              if (!condition_.wait_until(lock, until_, [this] { return complete_; })) {
                  expired_.store(true);
                  handle_.close();
              }
          }) {}

    ~RequestDeadline() {
        {
            std::lock_guard lock(mutex_);
            complete_ = true;
        }
        condition_.notify_one();
        worker_.join();
    }
    bool expired() const {
        return expired_.load() || std::chrono::steady_clock::now() >= until_;
    }

private:
    InternetHandle& handle_;
    const std::chrono::steady_clock::time_point until_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool complete_ = false;
    std::atomic<bool> expired_ = false;
    std::thread worker_;
};

void require(bool condition, const char* operation) {
    if (!condition) throw std::runtime_error(operation);
}

void create_session(InternetHandle& session) {
    session.set(WinHttpOpen(L"Codex Pulse CDP/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    require(session.get() != nullptr, "CDP HTTP session creation failed");
    require(WinHttpSetTimeouts(session.get(), 2000, 2000, 5000, 5000) != FALSE,
            "CDP HTTP timeout configuration failed");
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    require(WinHttpSetOption(session.get(), WINHTTP_OPTION_REDIRECT_POLICY,
                            &policy, sizeof(policy)) != FALSE,
            "CDP HTTP redirect configuration failed");
}

void create_request(InternetHandle& session, InternetHandle& connection,
                    InternetHandle& request, std::uint16_t port, const std::wstring& path) {
    create_session(session);
    connection.set(WinHttpConnect(session.get(), L"127.0.0.1", port, 0));
    require(connection.get() != nullptr, "CDP loopback connection failed");
    request.set(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
                                  WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
    require(request.get() != nullptr, "CDP HTTP request creation failed");
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_COOKIES;
    require(WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE,
                            &disabled, sizeof(disabled)) != FALSE,
            "CDP HTTP request configuration failed");
}

void send_request(InternetHandle& request, RequestDeadline& deadline, DWORD status) {
    require(WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                              WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE,
            "CDP HTTP request failed");
    require(WinHttpReceiveResponse(request.get(), nullptr) != FALSE,
            "CDP HTTP response failed");
    DWORD actual = 0;
    DWORD size = sizeof(actual);
    require(WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, &actual, &size,
                               WINHTTP_NO_HEADER_INDEX) != FALSE && actual == status,
            "CDP HTTP response status rejected");
    require(!deadline.expired(), "CDP HTTP request timed out");
}

struct PageEndpoint {
    std::uint16_t port;
    std::wstring path;
};

PageEndpoint page_endpoint(const std::string& url) {
    constexpr std::string_view prefix = "ws://127.0.0.1:";
    constexpr std::string_view page_prefix = "/devtools/page/";
    require(url.starts_with(prefix), "CDP WebSocket endpoint rejected");
    const auto slash = url.find('/', prefix.size());
    require(slash != std::string::npos && slash > prefix.size() && slash - prefix.size() <= 5,
            "CDP WebSocket endpoint rejected");
    unsigned port = 0;
    for (auto index = prefix.size(); index < slash; ++index) {
        const auto character = url[index];
        require(character >= '0' && character <= '9', "CDP WebSocket endpoint rejected");
        port = port * 10 + static_cast<unsigned>(character - '0');
    }
    require(port != 0 && port <= 65535, "CDP WebSocket endpoint rejected");
    const std::string_view path(url.data() + slash, url.size() - slash);
    require(path.starts_with(page_prefix) && path.size() > page_prefix.size() && path.size() <= 2048,
            "CDP WebSocket endpoint rejected");
    for (const char character : path.substr(page_prefix.size())) {
        require((character >= '0' && character <= '9') || (character >= 'a' && character <= 'z') ||
                (character >= 'A' && character <= 'Z') || character == '-' || character == '_',
                "CDP WebSocket endpoint rejected");
    }
    return {static_cast<std::uint16_t>(port), std::wstring(path.begin(), path.end())};
}

Json parse_message(const std::string& bytes) {
    auto parsed = Json::parse(bytes, nullptr, false);
    require(!parsed.is_discarded(), "CDP response JSON is invalid");
    return parsed;
}
}

Json cdp_targets(std::uint16_t port) {
    require(port != 0, "CDP loopback port rejected");
    InternetHandle session, connection, request;
    create_request(session, connection, request, port, L"/json/list");
    RequestDeadline deadline(request);
    send_request(request, deadline, 200);
    std::string body;
    std::array<char, 16 * 1024> buffer{};
    for (;;) {
        require(!deadline.expired(), "CDP HTTP request timed out");
        DWORD received = 0;
        require(WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
                                &received) != FALSE, "CDP HTTP response read failed");
        if (received == 0) break;
        require(body.size() <= message_limit - received, "CDP target response exceeded limit");
        body.append(buffer.data(), received);
    }
    require(!deadline.expired(), "CDP HTTP request timed out");
    auto result = parse_message(body);
    require(result.is_array(), "CDP target response is invalid");
    return result;
}

struct CdpClient::Impl {
    InternetHandle session;
    InternetHandle connection;
    InternetHandle socket;
    std::uint64_t next_id = 0;
};

CdpClient::CdpClient() : impl_(std::make_unique<Impl>()) {}
CdpClient::~CdpClient() { close(); }

bool CdpClient::connect(const std::string& websocket_url) {
    const auto endpoint = page_endpoint(websocket_url);
    InternetHandle session, connection, request;
    create_request(session, connection, request, endpoint.port, endpoint.path);
    require(WinHttpSetOption(request.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) != FALSE,
            "CDP WebSocket upgrade configuration failed");
    RequestDeadline deadline(request);
    send_request(request, deadline, 101);
    InternetHandle socket(WinHttpWebSocketCompleteUpgrade(request.get(), 0));
    require(socket.get() != nullptr && !deadline.expired(), "CDP WebSocket upgrade failed");
    close();
    impl_->session.set(session.release());
    impl_->connection.set(connection.release());
    impl_->socket.set(socket.release());
    return true;
}

Json CdpClient::command(const std::string& method, const Json& params) {
    require(impl_->socket.get() != nullptr, "CDP WebSocket is not connected");
    require(!method.empty() && params.is_object(), "CDP command request is invalid");
    require(impl_->next_id < std::numeric_limits<std::uint64_t>::max(), "CDP command id limit reached");
    const auto id = ++impl_->next_id;
    std::string bytes;
    try {
        bytes = Json{{"id", id}, {"method", method}, {"params", params}}.dump();
    } catch (const Json::exception&) {
        throw std::runtime_error("CDP command request JSON is invalid");
    }
    require(bytes.size() <= message_limit, "CDP command request exceeded limit");
    RequestDeadline deadline(impl_->socket);
    require(WinHttpWebSocketSend(impl_->socket.get(), WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                 bytes.data(), static_cast<DWORD>(bytes.size())) == NO_ERROR,
            "CDP WebSocket send failed");
    std::array<char, 16 * 1024> buffer{};
    std::string message;
    for (;;) {
        require(!deadline.expired(), "CDP command timed out");
        DWORD received = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        const auto status = WinHttpWebSocketReceive(impl_->socket.get(), buffer.data(),
            static_cast<DWORD>(buffer.size()), &received, &type);
        require(status == NO_ERROR, "CDP WebSocket receive failed");
        require(type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE ||
                type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                "CDP WebSocket message type rejected");
        require(message.size() <= message_limit - received, "CDP WebSocket response exceeded limit");
        message.append(buffer.data(), received);
        if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) continue;
        auto response = parse_message(message);
        message.clear();
        require(response.is_object(), "CDP response message is invalid");
        const auto response_id = response.find("id");
        if (response_id == response.end()) continue; // unsolicited event
        require(response_id->is_number_integer() || response_id->is_number_unsigned(),
                "CDP response id is invalid");
        if (*response_id != id) continue;
        require(!response.contains("error"), "CDP command was rejected");
        const auto result = response.find("result");
        require(result != response.end(), "CDP command result is missing");
        require(!deadline.expired(), "CDP command timed out");
        return *result;
    }
}

void CdpClient::close() {
    impl_->socket.close();
    impl_->connection.close();
    impl_->session.close();
    impl_->next_id = 0;
}
