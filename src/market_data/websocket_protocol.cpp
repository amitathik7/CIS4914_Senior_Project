#include "trading_engine/market_data/websocket.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace trading_engine::market_data {

IWebSocket::~IWebSocket() = default;

namespace ws {

namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

std::uint32_t rotl(std::uint32_t value, unsigned bits) { return (value << bits) | (value >> (32U - bits)); }

}  // namespace

Url parse_url(const std::string& url) {
    Url out;
    std::string_view rest{url};
    if (rest.substr(0, 6) == "wss://") {
        out.secure = true;
        rest.remove_prefix(6);
    } else if (rest.substr(0, 5) == "ws://") {
        rest.remove_prefix(5);
    } else {
        throw WebSocketError("WebSocket URL must start with ws:// or wss://: " + url);
    }

    const auto slash = rest.find('/');
    const std::string_view authority = rest.substr(0, slash);
    out.path = slash == std::string_view::npos ? "/" : std::string(rest.substr(slash));

    const auto colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
        const std::string_view port_text = authority.substr(colon + 1);
        unsigned port = 0;
        const auto [end, ec] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
        if (ec != std::errc{} || end != port_text.data() + port_text.size() || port == 0 || port > 65535) {
            throw WebSocketError("WebSocket URL has a bad port: " + url);
        }
        out.port = static_cast<std::uint16_t>(port);
        out.host = std::string(authority.substr(0, colon));
    } else {
        out.port = out.secure ? 443 : 80;
        out.host = std::string(authority);
    }
    if (out.host.empty()) {
        throw WebSocketError("WebSocket URL has no host: " + url);
    }
    return out;
}

std::string base64(std::string_view bytes) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const auto n = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i])) << 16U) |
                       (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i + 1])) << 8U) |
                       static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i + 2]));
        out += kAlphabet[(n >> 18U) & 63U];
        out += kAlphabet[(n >> 12U) & 63U];
        out += kAlphabet[(n >> 6U) & 63U];
        out += kAlphabet[n & 63U];
    }
    if (i + 1 == bytes.size()) {
        const auto n = static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i])) << 16U;
        out += kAlphabet[(n >> 18U) & 63U];
        out += kAlphabet[(n >> 12U) & 63U];
        out += "==";
    } else if (i + 2 == bytes.size()) {
        const auto n = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i])) << 16U) |
                       (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i + 1])) << 8U);
        out += kAlphabet[(n >> 18U) & 63U];
        out += kAlphabet[(n >> 12U) & 63U];
        out += kAlphabet[(n >> 6U) & 63U];
        out += '=';
    }
    return out;
}

// FIPS 180-1; only used for the handshake accept key
std::array<std::uint8_t, 20> sha1(std::string_view bytes) {
    std::uint32_t h[5] = {0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U};

    std::string message(bytes);
    const std::uint64_t bit_length = static_cast<std::uint64_t>(bytes.size()) * 8U;
    message.push_back(static_cast<char>(0x80));
    while (message.size() % 64 != 56) {
        message.push_back('\0');
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        message.push_back(static_cast<char>((bit_length >> static_cast<unsigned>(shift)) & 0xFFU));
    }

    for (std::size_t chunk = 0; chunk < message.size(); chunk += 64) {
        std::uint32_t w[80];
        for (std::size_t i = 0; i < 16; ++i) {
            const auto* p = reinterpret_cast<const unsigned char*>(message.data() + chunk + i * 4);
            w[i] = (static_cast<std::uint32_t>(p[0]) << 24U) | (static_cast<std::uint32_t>(p[1]) << 16U) |
                   (static_cast<std::uint32_t>(p[2]) << 8U) | static_cast<std::uint32_t>(p[3]);
        }
        for (std::size_t i = 16; i < 80; ++i) {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (std::size_t i = 0; i < 80; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999U;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1U;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCU;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6U;
            }
            const std::uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = temp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    std::array<std::uint8_t, 20> digest{};
    for (std::size_t i = 0; i < 5; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>(h[i] >> 24U);
        digest[i * 4 + 1] = static_cast<std::uint8_t>(h[i] >> 16U);
        digest[i * 4 + 2] = static_cast<std::uint8_t>(h[i] >> 8U);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(h[i]);
    }
    return digest;
}

std::string accept_key(std::string_view client_key) {
    const auto digest = sha1(std::string(client_key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    return base64(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
}

std::string handshake_request(const Url& url, std::string_view client_key,
                              const std::vector<std::pair<std::string, std::string>>& headers) {
    const bool default_port = url.port == (url.secure ? 443 : 80);
    std::string request = "GET " + url.path + " HTTP/1.1\r\n";
    request += "Host: " + url.host + (default_port ? "" : ":" + std::to_string(url.port)) + "\r\n";
    request += "Upgrade: websocket\r\nConnection: Upgrade\r\n";
    request += "Sec-WebSocket-Key: " + std::string(client_key) + "\r\n";
    request += "Sec-WebSocket-Version: 13\r\n";
    for (const auto& [name, value] : headers) {
        request += name + ": " + value + "\r\n";
    }
    request += "\r\n";
    return request;
}

void check_handshake_response(std::string_view head, std::string_view client_key) {
    const auto line_end = head.find("\r\n");
    const std::string_view status_line = head.substr(0, line_end);
    const auto space = status_line.find(' ');
    if (status_line.substr(0, 5) != "HTTP/" || space == std::string_view::npos ||
        status_line.substr(space + 1, 3) != "101") {
        throw WebSocketError("WebSocket handshake refused: " + std::string(status_line));
    }

    bool upgrade = false;
    std::string accept;
    std::size_t pos = line_end == std::string_view::npos ? head.size() : line_end + 2;
    while (pos < head.size()) {
        const auto end = head.find("\r\n", pos);
        const std::string_view line = head.substr(pos, end == std::string_view::npos ? head.size() - pos : end - pos);
        pos = end == std::string_view::npos ? head.size() : end + 2;
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        const std::string name = lower(trim(line.substr(0, colon)));
        const std::string_view value = trim(line.substr(colon + 1));
        if (name == "upgrade") {
            upgrade = lower(value) == "websocket";
        } else if (name == "sec-websocket-accept") {
            accept = std::string(value);
        }
    }

    if (!upgrade) {
        throw WebSocketError("WebSocket handshake: response has no 'Upgrade: websocket'");
    }
    if (accept != accept_key(client_key)) {
        throw WebSocketError("WebSocket handshake: Sec-WebSocket-Accept does not match the key");
    }
}

std::string encode_frame(Opcode opcode, std::string_view payload, std::array<std::uint8_t, 4> mask, bool fin) {
    std::string frame;
    frame.push_back(static_cast<char>((fin ? 0x80U : 0x00U) | static_cast<unsigned>(opcode)));

    const std::uint64_t length = payload.size();
    if (length < 126) {
        frame.push_back(static_cast<char>(0x80U | length));
    } else if (length <= 0xFFFF) {
        frame.push_back(static_cast<char>(0x80U | 126U));
        frame.push_back(static_cast<char>((length >> 8U) & 0xFFU));
        frame.push_back(static_cast<char>(length & 0xFFU));
    } else {
        frame.push_back(static_cast<char>(0x80U | 127U));
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<char>((length >> static_cast<unsigned>(shift)) & 0xFFU));
        }
    }

    for (const auto byte : mask) {
        frame.push_back(static_cast<char>(byte));
    }
    for (std::size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ mask[i % 4]));
    }
    return frame;
}

std::optional<Frame> FrameDecoder::next() {
    if (buffer_.size() < 2) {
        return std::nullopt;
    }
    const auto byte = [this](std::size_t i) { return static_cast<std::uint8_t>(buffer_[i]); };

    const std::uint8_t b0 = byte(0);
    const std::uint8_t b1 = byte(1);
    if ((b0 & 0x70U) != 0) {
        throw WebSocketError("WebSocket frame uses reserved bits");
    }
    if ((b1 & 0x80U) != 0) {
        throw WebSocketError("WebSocket server frame is masked");
    }

    const auto opcode_bits = static_cast<std::uint8_t>(b0 & 0x0FU);
    const bool known = opcode_bits <= 0x2 || (opcode_bits >= 0x8 && opcode_bits <= 0xA);
    if (!known) {
        throw WebSocketError("WebSocket frame has unknown opcode " + std::to_string(opcode_bits));
    }

    std::size_t header = 2;
    std::uint64_t length = b1 & 0x7FU;
    if (length == 126) {
        if (buffer_.size() < 4) {
            return std::nullopt;
        }
        length = (static_cast<std::uint64_t>(byte(2)) << 8U) | byte(3);
        header = 4;
    } else if (length == 127) {
        if (buffer_.size() < 10) {
            return std::nullopt;
        }
        length = 0;
        for (std::size_t i = 2; i < 10; ++i) {
            length = (length << 8U) | byte(i);
        }
        header = 10;
    }

    const bool fin = (b0 & 0x80U) != 0;
    const auto opcode = static_cast<Opcode>(opcode_bits);
    if (opcode_bits >= 0x8 && (!fin || length > 125)) {
        throw WebSocketError("WebSocket control frame is fragmented or longer than 125 bytes");
    }
    if (length > max_payload_) {
        throw WebSocketError("WebSocket frame of " + std::to_string(length) + " bytes exceeds the limit");
    }
    if (buffer_.size() < header + length) {
        return std::nullopt;
    }

    Frame frame{fin, opcode, buffer_.substr(header, static_cast<std::size_t>(length))};
    buffer_.erase(0, header + static_cast<std::size_t>(length));
    return frame;
}

}  // namespace ws

}  // namespace trading_engine::market_data
