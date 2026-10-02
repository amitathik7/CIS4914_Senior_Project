#pragma once

// -----------------------------------------------------------------------------
//  JsonWriter -- a small, strict, deterministic JSON emitter. No JSON library is used
//  anywhere in the project (docs/adr/0002 section 9), and the lab needs only to WRITE.
//
//  Guarantees (tested, and checked with a real JSON parser in tests/strategy_lab):
//   * Output is RFC 8259 JSON in UTF-8. Members appear in the order written; nothing
//     is sorted or hashed, so the same calls always give the same bytes.
//   * Strings must be valid UTF-8 (std::invalid_argument otherwise). `"`, `\` and every
//     control character below 0x20, plus DEL and U+2028 / U+2029, are escaped; the rest
//     is written as is.
//   * Numbers are written with std::to_chars: the shortest text that reads back as the
//     exact same double, independent of any locale (global C++ locale or setlocale).
//     NaN and infinity cannot be written (std::domain_error): JSON has no spelling for
//     them, and callers say "unavailable" explicitly instead.
//   * Misuse (a value with no key inside an object, a key inside an array, closing the
//     wrong container, a second root value, reading an incomplete document) throws
//     std::logic_error instead of producing broken JSON.
//   * Line endings are always "\n". In pretty mode each level is indented two spaces.
//
//  Not thread-safe.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace trading_engine::lab {

class JsonWriter {
public:
    explicit JsonWriter(bool pretty = false, std::size_t base_depth = 0)
        : pretty_{pretty}, base_depth_{base_depth} {}

    JsonWriter& begin_object();
    JsonWriter& end_object();
    JsonWriter& begin_array();
    JsonWriter& end_array();
    JsonWriter& key(std::string_view name);

    JsonWriter& string(std::string_view value);
    JsonWriter& boolean(bool value);
    JsonWriter& null();
    JsonWriter& integer(std::int64_t value);
    JsonWriter& unsigned_integer(std::uint64_t value);
    JsonWriter& number(double value);

    // key + value in one call.
    JsonWriter& field(std::string_view name, std::string_view value) { return key(name).string(value); }
    JsonWriter& field(std::string_view name, const char* value) { return key(name).string(value); }
    JsonWriter& field(std::string_view name, bool value) { return key(name).boolean(value); }
    JsonWriter& field(std::string_view name, double value) { return key(name).number(value); }
    JsonWriter& field(std::string_view name, std::uint64_t value) { return key(name).unsigned_integer(value); }
    JsonWriter& field(std::string_view name, std::int64_t value) { return key(name).integer(value); }
    JsonWriter& field(std::string_view name, int value) { return key(name).integer(value); }
    // A std::size_t is a std::uint64_t on the 64-bit targets: no separate overload.

    // True once exactly one complete root value has been written.
    [[nodiscard]] bool complete() const noexcept { return root_done_ && stack_.empty(); }

    // The document so far; throws std::logic_error unless complete().
    [[nodiscard]] const std::string& str() const;
    [[nodiscard]] std::string take();

private:
    struct Frame {
        bool is_object;
        bool first;       // no element written yet
        bool have_key;    // an object member's key is waiting for its value
    };

    void before_value();
    void newline(std::size_t depth);
    void write_string(std::string_view value);

    bool               pretty_;
    std::size_t        base_depth_;
    bool               root_done_{false};
    std::string        out_{};
    std::vector<Frame> stack_{};
};

}  // namespace trading_engine::lab
