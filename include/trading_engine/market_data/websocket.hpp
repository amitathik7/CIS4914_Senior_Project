#pragma once

// Minimal blocking WebSocket client (RFC 6455) used by the Alpaca live source.
//
// make_default_websocket() returns a client that opens the TCP/TLS connection
// with libcurl's connect-only mode and does the WebSocket handshake and
// framing itself (websocket_protocol.cpp), so it needs no WebSocket support
// in libcurl. Without libcurl it returns a client whose connect() throws.
//
// The protocol pieces are exposed in `ws` for unit tests.

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trading_engine::market_data {

// Connection, handshake or framing failure; the connection is unusable.
class WebSocketError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The server closed the connection with a close frame.
class WebSocketClosed : public WebSocketError {
public:
    WebSocketClosed(std::uint16_t code, const std::string& reason)
        : WebSocketError("WebSocket closed by server (" + std::to_string(code) +
                         (reason.empty() ? "" : ": " + reason) + ")"),
          code_{code} {}

    [[nodiscard]] std::uint16_t code() const noexcept { return code_; }

private:
    std::uint16_t code_;
};

class IWebSocket {
public:
    virtual ~IWebSocket();

    // Opens ws:// or wss:// `url` and completes the handshake.
    virtual void connect(const std::string& url,
                         const std::vector<std::pair<std::string, std::string>>& headers) = 0;

    virtual void send_text(std::string_view message) = 0;

    // One complete text or binary message, or nullopt when `timeout` passes
    // first. Pings are answered inside. Throws WebSocketClosed / WebSocketError.
    [[nodiscard]] virtual std::optional<std::string> receive(std::chrono::milliseconds timeout) = 0;

    // Sends a close frame if still open and drops the connection.
    virtual void close() noexcept = 0;
};

[[nodiscard]] std::unique_ptr<IWebSocket> make_default_websocket();

// true when make_default_websocket() can make real connections
[[nodiscard]] bool websocket_available() noexcept;

namespace ws {

enum class Opcode : std::uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

struct Frame {
    bool fin{true};
    Opcode opcode{Opcode::Text};
    std::string payload;
};

struct Url {
    bool secure{false};
    std::string host;
    std::uint16_t port{0};
    std::string path;   // includes the query, at least "/"
};

// Throws WebSocketError for anything but ws:// or wss://host[:port][/path].
[[nodiscard]] Url parse_url(const std::string& url);

[[nodiscard]] std::string base64(std::string_view bytes);
[[nodiscard]] std::array<std::uint8_t, 20> sha1(std::string_view bytes);

// Sec-WebSocket-Accept for a Sec-WebSocket-Key (RFC 6455 section 4.2.2).
[[nodiscard]] std::string accept_key(std::string_view client_key);

[[nodiscard]] std::string handshake_request(const Url& url, std::string_view client_key,
                                            const std::vector<std::pair<std::string, std::string>>& headers);

// Checks the response head (up to and including the blank line): status 101,
// Upgrade: websocket, and the expected accept key. Throws WebSocketError.
void check_handshake_response(std::string_view head, std::string_view client_key);

// A client frame: always masked with `mask`.
[[nodiscard]] std::string encode_frame(Opcode opcode, std::string_view payload,
                                       std::array<std::uint8_t, 4> mask, bool fin = true);

// Incremental decoder for server frames. feed() any number of bytes, then
// call next() until it returns nullopt. Throws WebSocketError on a protocol
// violation (masked server frame, reserved bits, oversized or fragmented
// control frame, frame longer than max_payload).
class FrameDecoder {
public:
    explicit FrameDecoder(std::size_t max_payload = 16U << 20U) : max_payload_{max_payload} {}

    void feed(std::string_view bytes) { buffer_.append(bytes); }
    [[nodiscard]] std::optional<Frame> next();

private:
    std::string buffer_;
    std::size_t max_payload_;
};

}  // namespace ws

}  // namespace trading_engine::market_data
