#pragma once

// A deliberately small JSON reader/writer, private to the event codec.
//
// The core library takes no third-party dependencies (src/CMakeLists.txt), and
// the wire format only needs objects, arrays, strings, integers, doubles and
// booleans. Numbers are kept as their source text so integers round-trip
// exactly (uint64 sequences, int64 quantities) and doubles go through
// std::from_chars. Not a general-purpose parser: no streaming, depth limited.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trading_engine::events::json {

struct Value {
    enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };

    Kind kind{Kind::Null};
    bool boolean{false};
    std::string text{};   // Number: the literal; String: the decoded UTF-8
    std::vector<Value> array{};
    std::map<std::string, Value, std::less<>> object{};

    [[nodiscard]] const Value* find(std::string_view key) const;
};

// nullopt on malformed input, trailing garbage, or nesting deeper than 32.
[[nodiscard]] std::optional<Value> parse(std::string_view text);

// Writes `text` as a JSON string literal (quotes included).
void append_string(std::string& out, std::string_view text);

// Builds one JSON object incrementally: {"a":1,"b":"x"}.
class ObjectWriter {
public:
    explicit ObjectWriter(std::string& out);
    void key(std::string_view name);
    void string(std::string_view name, std::string_view value);
    void raw(std::string_view name, std::string_view literal);   // pre-encoded JSON
    void integer(std::string_view name, std::int64_t value);
    void unsigned_integer(std::string_view name, std::uint64_t value);
    void number(std::string_view name, double value);            // shortest round-trip text
    void close();

private:
    std::string& out_;
    bool first_{true};
};

}  // namespace trading_engine::events::json
