#include "trading_engine/market_data/websocket.hpp"

#include <curl/curl.h>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <poll.h>
#endif

#include <algorithm>
#include <mutex>
#include <random>

// libcurl opens the TCP/TLS connection (CURLOPT_CONNECT_ONLY) and moves raw
// bytes; the handshake and framing are websocket_protocol.cpp's.

namespace trading_engine::market_data {

namespace {

using Clock = std::chrono::steady_clock;

// waits up to `timeout` for the socket to become readable (or writable)
bool wait_socket(curl_socket_t socket, bool for_read, std::chrono::milliseconds timeout) {
    const int ms = static_cast<int>(std::max<std::chrono::milliseconds::rep>(0, timeout.count()));
#if defined(_WIN32)
    WSAPOLLFD fd{};
    fd.fd = socket;
    fd.events = for_read ? POLLRDNORM : POLLWRNORM;
    return WSAPoll(&fd, 1, ms) > 0;
#else
    pollfd fd{};
    fd.fd = socket;
    fd.events = for_read ? POLLIN : POLLOUT;
    return ::poll(&fd, 1, ms) > 0;
#endif
}

class CurlWebSocket final : public IWebSocket {
public:
    CurlWebSocket() : random_{std::random_device{}()} {
        static std::once_flag global_init;
        std::call_once(global_init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    }

    ~CurlWebSocket() override { close(); }

    void connect(const std::string& url,
                 const std::vector<std::pair<std::string, std::string>>& headers) override {
        close();
        const ws::Url target = ws::parse_url(url);

        curl_ = curl_easy_init();
        if (curl_ == nullptr) {
            throw WebSocketError("curl_easy_init failed");
        }
        const std::string transport = std::string(target.secure ? "https://" : "http://") + target.host + ":" +
                                      std::to_string(target.port) + "/";
        char error[CURL_ERROR_SIZE] = {};
        curl_easy_setopt(curl_, CURLOPT_URL, transport.c_str());
        curl_easy_setopt(curl_, CURLOPT_CONNECT_ONLY, 1L);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl_, CURLOPT_ERRORBUFFER, error);
        curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
        // the WebSocket upgrade is HTTP/1.1 only
        curl_easy_setopt(curl_, CURLOPT_SSL_ENABLE_ALPN, 0L);

        const CURLcode code = curl_easy_perform(curl_);
        if (code != CURLE_OK) {
            const std::string why = error[0] != '\0' ? error : curl_easy_strerror(code);
            close();
            throw WebSocketError("WebSocket connect to " + target.host + " failed: " + why);
        }
        curl_easy_getinfo(curl_, CURLINFO_ACTIVESOCKET, &socket_);

        std::string nonce(16, '\0');
        for (auto& c : nonce) {
            c = static_cast<char>(random_() & 0xFFU);
        }
        const std::string key = ws::base64(nonce);
        send_all(ws::handshake_request(target, key, headers));

        std::string head;
        const auto deadline = Clock::now() + std::chrono::seconds{15};
        std::size_t end = std::string::npos;
        while ((end = head.find("\r\n\r\n")) == std::string::npos) {
            if (head.size() > 16384 || !read_some(head, deadline)) {
                close();
                throw WebSocketError("WebSocket handshake: no complete response from " + target.host);
            }
        }
        try {
            ws::check_handshake_response(std::string_view(head).substr(0, end + 4), key);
        } catch (...) {
            close();
            throw;
        }
        decoder_ = ws::FrameDecoder{};
        decoder_.feed(std::string_view(head).substr(end + 4));
        message_.clear();
        in_message_ = false;
        open_ = true;
    }

    void send_text(std::string_view message) override {
        require_open();
        send_frame(ws::Opcode::Text, message);
    }

    std::optional<std::string> receive(std::chrono::milliseconds timeout) override {
        require_open();
        const auto deadline = Clock::now() + timeout;
        for (;;) {
            while (auto frame = decoder_.next()) {
                if (auto message = handle(std::move(*frame))) {
                    return message;
                }
            }
            std::string bytes;
            if (!read_some(bytes, deadline)) {
                return std::nullopt;
            }
            decoder_.feed(bytes);
        }
    }

    void close() noexcept override {
        if (curl_ == nullptr) {
            return;
        }
        if (open_) {
            open_ = false;
            try {
                send_frame(ws::Opcode::Close, std::string("\x03\xE8", 2));
            } catch (...) {
            }
        }
        curl_easy_cleanup(curl_);
        curl_ = nullptr;
        socket_ = CURL_SOCKET_BAD;
    }

private:
    void require_open() const {
        if (!open_) {
            throw WebSocketError("WebSocket is not connected");
        }
    }

    std::array<std::uint8_t, 4> mask() {
        const auto bits = static_cast<std::uint32_t>(random_());
        return {static_cast<std::uint8_t>(bits >> 24U), static_cast<std::uint8_t>(bits >> 16U),
                static_cast<std::uint8_t>(bits >> 8U), static_cast<std::uint8_t>(bits)};
    }

    void send_frame(ws::Opcode opcode, std::string_view payload) {
        send_all(ws::encode_frame(opcode, payload, mask()));
    }

    void send_all(std::string_view data) {
        const auto deadline = Clock::now() + std::chrono::seconds{10};
        while (!data.empty()) {
            std::size_t sent = 0;
            const CURLcode code = curl_easy_send(curl_, data.data(), data.size(), &sent);
            if (code == CURLE_OK) {
                data.remove_prefix(sent);
                continue;
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            if (code != CURLE_AGAIN || left.count() <= 0 || !wait_socket(socket_, false, left)) {
                open_ = false;
                throw WebSocketError(std::string("WebSocket send failed: ") +
                                     (code == CURLE_AGAIN ? "timed out" : curl_easy_strerror(code)));
            }
        }
    }

    // appends what is available, waiting until `deadline`; false on timeout
    bool read_some(std::string& out, Clock::time_point deadline) {
        char buffer[16384];
        for (;;) {
            std::size_t got = 0;
            const CURLcode code = curl_easy_recv(curl_, buffer, sizeof buffer, &got);
            if (code == CURLE_OK) {
                if (got == 0) {
                    open_ = false;
                    throw WebSocketError("WebSocket connection closed without a close frame");
                }
                out.append(buffer, got);
                return true;
            }
            if (code != CURLE_AGAIN) {
                open_ = false;
                throw WebSocketError(std::string("WebSocket receive failed: ") + curl_easy_strerror(code));
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            if (left.count() <= 0 || !wait_socket(socket_, true, left)) {
                return false;
            }
        }
    }

    std::optional<std::string> handle(ws::Frame frame) {
        switch (frame.opcode) {
            case ws::Opcode::Ping:
                send_frame(ws::Opcode::Pong, frame.payload);
                return std::nullopt;
            case ws::Opcode::Pong:
                return std::nullopt;
            case ws::Opcode::Close: {
                std::uint16_t code = 1005;
                std::string reason;
                if (frame.payload.size() >= 2) {
                    code = static_cast<std::uint16_t>((static_cast<std::uint8_t>(frame.payload[0]) << 8U) |
                                                      static_cast<std::uint8_t>(frame.payload[1]));
                    reason = frame.payload.substr(2);
                }
                close();
                throw WebSocketClosed(code, reason);
            }
            case ws::Opcode::Text:
            case ws::Opcode::Binary:
                if (in_message_) {
                    throw WebSocketError("WebSocket data frame inside a fragmented message");
                }
                if (frame.fin) {
                    return std::move(frame.payload);
                }
                message_ = std::move(frame.payload);
                in_message_ = true;
                return std::nullopt;
            case ws::Opcode::Continuation:
                if (!in_message_) {
                    throw WebSocketError("WebSocket continuation frame without a message");
                }
                message_ += frame.payload;
                if (frame.fin) {
                    in_message_ = false;
                    return std::move(message_);
                }
                return std::nullopt;
        }
        return std::nullopt;
    }

    CURL* curl_{nullptr};
    curl_socket_t socket_{CURL_SOCKET_BAD};
    bool open_{false};
    ws::FrameDecoder decoder_;
    std::string message_;
    bool in_message_{false};
    std::mt19937 random_;
};

}  // namespace

std::unique_ptr<IWebSocket> make_default_websocket() { return std::make_unique<CurlWebSocket>(); }

bool websocket_available() noexcept { return true; }

}  // namespace trading_engine::market_data
