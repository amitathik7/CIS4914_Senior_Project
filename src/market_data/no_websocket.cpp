#include "trading_engine/market_data/websocket.hpp"

// Compiled instead of curl_websocket.cpp when CMake found no libcurl.

namespace trading_engine::market_data {

namespace {

class UnavailableWebSocket final : public IWebSocket {
public:
    void connect(const std::string&, const std::vector<std::pair<std::string, std::string>>&) override {
        throw WebSocketError(
            "this build has no WebSocket client: install libcurl (brew install curl / "
            "apt install libcurl4-openssl-dev / vcpkg install curl) and reconfigure");
    }
    void send_text(std::string_view) override { throw WebSocketError("WebSocket is not connected"); }
    std::optional<std::string> receive(std::chrono::milliseconds) override {
        throw WebSocketError("WebSocket is not connected");
    }
    void close() noexcept override {}
};

}  // namespace

std::unique_ptr<IWebSocket> make_default_websocket() { return std::make_unique<UnavailableWebSocket>(); }

bool websocket_available() noexcept { return false; }

}  // namespace trading_engine::market_data
