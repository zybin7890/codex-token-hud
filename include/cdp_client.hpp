#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

using Json = nlohmann::json;

// Fetches only the caller-selected loopback CDP endpoint. No target content or
// titles are logged by this helper.
Json cdp_targets(std::uint16_t port);

class CdpClient {
public:
    CdpClient();
    ~CdpClient();
    CdpClient(const CdpClient&) = delete;
    CdpClient& operator=(const CdpClient&) = delete;

    bool connect(const std::string& websocket_url);
    Json command(const std::string& method, const Json& params = Json::object());
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
