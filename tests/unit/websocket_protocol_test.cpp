// RFC 6455 pieces of the WebSocket client: URL parsing, the handshake, SHA-1
// and base64 for the accept key, and frame encoding/decoding. Vectors are from
// RFC 6455, RFC 4648 and FIPS 180-1.

#include <gtest/gtest.h>

#include <string>

#include "trading_engine/market_data/websocket.hpp"

namespace {

namespace md = trading_engine::market_data;
namespace ws = trading_engine::market_data::ws;

std::string bytes(std::initializer_list<int> values) {
    std::string out;
    for (const int v : values) {
        out.push_back(static_cast<char>(v));
    }
    return out;
}

std::string hex(const std::array<std::uint8_t, 20>& digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (const auto b : digest) {
        out += kHex[b >> 4U];
        out += kHex[b & 0xFU];
    }
    return out;
}

TEST(WebSocketUrl, SplitsSchemeHostPortAndPath) {
    const auto a = ws::parse_url("wss://stream.data.alpaca.markets/v2/iex");
    EXPECT_TRUE(a.secure);
    EXPECT_EQ(a.host, "stream.data.alpaca.markets");
    EXPECT_EQ(a.port, 443);
    EXPECT_EQ(a.path, "/v2/iex");

    const auto b = ws::parse_url("ws://127.0.0.1:18766");
    EXPECT_FALSE(b.secure);
    EXPECT_EQ(b.host, "127.0.0.1");
    EXPECT_EQ(b.port, 18766);
    EXPECT_EQ(b.path, "/");

    for (const char* bad : {"https://x/", "wss://", "ws://host:0/", "ws://host:99999/", "ws://host:12a/"}) {
        EXPECT_THROW((void)ws::parse_url(bad), md::WebSocketError) << bad;
    }
}

TEST(WebSocketHashing, MatchesTheStandardVectors) {
    EXPECT_EQ(ws::base64(""), "");
    EXPECT_EQ(ws::base64("f"), "Zg==");
    EXPECT_EQ(ws::base64("fo"), "Zm8=");
    EXPECT_EQ(ws::base64("foo"), "Zm9v");
    EXPECT_EQ(ws::base64("foobar"), "Zm9vYmFy");

    EXPECT_EQ(hex(ws::sha1("")), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    EXPECT_EQ(hex(ws::sha1("abc")), "a9993e364706816aba3e25717850c26c9cd0d89d");
    EXPECT_EQ(hex(ws::sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
              "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    EXPECT_EQ(hex(ws::sha1(std::string(1000, 'a'))), "291e9a6c66994949b57ba5e650361e98fc36b1ba");

    EXPECT_EQ(ws::accept_key("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(WebSocketHandshake, RequestCarriesTheUpgradeHeadersAndExtras) {
    const auto request = ws::handshake_request(ws::parse_url("ws://localhost:8080/feed?x=1"), "KEY==",
                                               {{"Authorization", "Bearer t"}});
    EXPECT_EQ(request.rfind("GET /feed?x=1 HTTP/1.1\r\n", 0), 0u);
    EXPECT_NE(request.find("Host: localhost:8080\r\n"), std::string::npos);
    EXPECT_NE(request.find("Upgrade: websocket\r\n"), std::string::npos);
    EXPECT_NE(request.find("Connection: Upgrade\r\n"), std::string::npos);
    EXPECT_NE(request.find("Sec-WebSocket-Key: KEY==\r\n"), std::string::npos);
    EXPECT_NE(request.find("Sec-WebSocket-Version: 13\r\n"), std::string::npos);
    EXPECT_NE(request.find("Authorization: Bearer t\r\n"), std::string::npos);
    EXPECT_EQ(request.substr(request.size() - 4), "\r\n\r\n");

    const auto standard = ws::handshake_request(ws::parse_url("wss://example.com/"), "K", {});
    EXPECT_NE(standard.find("Host: example.com\r\n"), std::string::npos);
}

TEST(WebSocketHandshake, AcceptsOnlyA101WithTheMatchingAcceptKey) {
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    EXPECT_NO_THROW(ws::check_handshake_response("HTTP/1.1 101 Switching Protocols\r\n"
                                                 "upgrade: WebSocket\r\nConnection: Upgrade\r\n"
                                                 "Sec-WebSocket-Accept:  s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n",
                                                 key));
    EXPECT_THROW(ws::check_handshake_response("HTTP/1.1 401 Unauthorized\r\n\r\n", key), md::WebSocketError);
    EXPECT_THROW(ws::check_handshake_response("HTTP/1.1 101 OK\r\n"
                                              "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n",
                                              key),
                 md::WebSocketError);
    EXPECT_THROW(ws::check_handshake_response("HTTP/1.1 101 OK\r\nUpgrade: websocket\r\n"
                                              "Sec-WebSocket-Accept: wrong=\r\n\r\n",
                                              key),
                 md::WebSocketError);
}

TEST(WebSocketFrames, ClientFramesAreMaskedWithTheRightLengthEncoding) {
    // RFC 6455 5.7: a masked "Hello"
    EXPECT_EQ(ws::encode_frame(ws::Opcode::Text, "Hello", {0x37, 0xfa, 0x21, 0x3d}),
              bytes({0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58}));

    const auto medium = ws::encode_frame(ws::Opcode::Binary, std::string(300, 'x'), {1, 2, 3, 4});
    EXPECT_EQ(medium.substr(0, 4), bytes({0x82, 0xFE, 0x01, 0x2C}));
    EXPECT_EQ(medium.size(), 4u + 4u + 300u);

    const auto large = ws::encode_frame(ws::Opcode::Text, std::string(70000, 'y'), {0, 0, 0, 0}, false);
    EXPECT_EQ(large.substr(0, 10), bytes({0x01, 0xFF, 0, 0, 0, 0, 0, 0x01, 0x11, 0x70}));
}

TEST(WebSocketFrames, DecoderHandlesPartialInputExtendedLengthsAndFragments) {
    ws::FrameDecoder decoder;
    const std::string hello = bytes({0x81, 0x05, 'H', 'e', 'l', 'l', 'o'});
    for (std::size_t i = 0; i + 1 < hello.size(); ++i) {
        decoder.feed(hello.substr(i, 1));
        EXPECT_FALSE(decoder.next().has_value()) << i;
    }
    decoder.feed(hello.substr(hello.size() - 1));
    auto frame = decoder.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_TRUE(frame->fin);
    EXPECT_EQ(frame->opcode, ws::Opcode::Text);
    EXPECT_EQ(frame->payload, "Hello");

    decoder.feed(bytes({0x01, 0x03, 'H', 'e', 'l', 0x80, 0x02, 'l', 'o', 0x89, 0x00}));
    frame = decoder.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_FALSE(frame->fin);
    EXPECT_EQ(frame->payload, "Hel");
    frame = decoder.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->opcode, ws::Opcode::Continuation);
    EXPECT_EQ(frame->payload, "lo");
    frame = decoder.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->opcode, ws::Opcode::Ping);
    EXPECT_FALSE(decoder.next().has_value());

    decoder.feed(bytes({0x82, 0x7E, 0x01, 0x00}) + std::string(256, 'z'));
    frame = decoder.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->payload.size(), 256u);

    decoder.feed(bytes({0x82, 0x7F, 0, 0, 0, 0, 0, 0x01, 0, 0}) + std::string(65536, 'w'));
    frame = decoder.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->payload.size(), 65536u);
}

TEST(WebSocketFrames, DecoderRejectsProtocolViolations) {
    const auto rejects = [](const std::string& input, std::size_t limit = 1U << 20U) {
        ws::FrameDecoder decoder{limit};
        decoder.feed(input);
        EXPECT_THROW((void)decoder.next(), md::WebSocketError);
    };
    rejects(bytes({0x81, 0x81, 1, 2, 3, 4, 'x'}));          // masked server frame
    rejects(bytes({0xC1, 0x00}));                             // reserved bit
    rejects(bytes({0x83, 0x00}));                             // unknown opcode
    rejects(bytes({0x09, 0x00}));                             // fragmented ping
    rejects(bytes({0x89, 0x7E, 0x00, 0x7E}));                 // control frame over 125 bytes
    rejects(bytes({0x82, 0x7E, 0x10, 0x00}), 1024);           // larger than the limit
}

}  // namespace
