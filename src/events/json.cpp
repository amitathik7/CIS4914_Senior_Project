#include "json.hpp"

#include <charconv>
#include <cmath>
#include <stdexcept>

namespace trading_engine::events::json {

const Value* Value::find(std::string_view key) const {
    if (kind != Kind::Object) {
        return nullptr;
    }
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &it->second;
}

namespace {

constexpr int kMaxDepth = 32;

class Parser {
public:
    explicit Parser(std::string_view text) : text_{text} {}

    std::optional<Value> document() {
        Value value;
        skip_ws();
        if (!parse_value(value, 0)) {
            return std::nullopt;
        }
        skip_ws();
        if (pos_ != text_.size()) {
            return std::nullopt;
        }
        return value;
    }

private:
    void skip_ws() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' ||
                text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) {
            return false;
        }
        pos_ += word.size();
        return true;
    }

    bool parse_value(Value& out, int depth) {
        if (depth > kMaxDepth || pos_ >= text_.size()) {
            return false;
        }
        const char c = text_[pos_];
        if (c == '{') {
            return parse_object(out, depth);
        }
        if (c == '[') {
            return parse_array(out, depth);
        }
        if (c == '"') {
            out.kind = Value::Kind::String;
            return parse_string(out.text);
        }
        if (c == 't' || c == 'f') {
            out.kind    = Value::Kind::Bool;
            out.boolean = (c == 't');
            return literal(out.boolean ? "true" : "false");
        }
        if (c == 'n') {
            out.kind = Value::Kind::Null;
            return literal("null");
        }
        return parse_number(out);
    }

    bool parse_object(Value& out, int depth) {
        out.kind = Value::Kind::Object;
        ++pos_;   // '{'
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skip_ws();
            std::string key;
            if (pos_ >= text_.size() || text_[pos_] != '"' || !parse_string(key)) {
                return false;
            }
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                return false;
            }
            ++pos_;
            skip_ws();
            Value member;
            if (!parse_value(member, depth + 1)) {
                return false;
            }
            if (!out.object.emplace(std::move(key), std::move(member)).second) {
                return false;   // duplicate keys are ambiguous: refuse them
            }
            skip_ws();
            if (pos_ >= text_.size()) {
                return false;
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return false;
        }
    }

    bool parse_array(Value& out, int depth) {
        out.kind = Value::Kind::Array;
        ++pos_;   // '['
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            skip_ws();
            Value element;
            if (!parse_value(element, depth + 1)) {
                return false;
            }
            out.array.push_back(std::move(element));
            skip_ws();
            if (pos_ >= text_.size()) {
                return false;
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return false;
        }
    }

    bool hex4(std::uint32_t& out) {
        if (pos_ + 4 > text_.size()) {
            return false;
        }
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            value <<= 4U;
            if (c >= '0' && c <= '9') {
                value |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        out = value;
        return true;
    }

    static void append_utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parse_string(std::string& out) {
        ++pos_;   // opening quote
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return false;   // raw control characters are not allowed
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) {
                return false;
            }
            const char escape = text_[pos_++];
            switch (escape) {
                case '"':  out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/'); break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!hex4(cp)) {
                        return false;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF) {   // high surrogate: need a low one
                        std::uint32_t low = 0;
                        if (pos_ + 2 > text_.size() || text_[pos_] != '\\' ||
                            text_[pos_ + 1] != 'u') {
                            return false;
                        }
                        pos_ += 2;
                        if (!hex4(low) || low < 0xDC00 || low > 0xDFFF) {
                            return false;
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10U) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return false;   // lone low surrogate
                    }
                    append_utf8(out, cp);
                    break;
                }
                default:
                    return false;
            }
        }
        return false;   // unterminated
    }

    bool parse_number(Value& out) {
        const std::size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') {
            ++pos_;
        }
        const auto digits = [&] {
            const std::size_t from = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                ++pos_;
            }
            return pos_ - from;
        };
        const std::size_t int_start = pos_;
        const std::size_t int_digits = digits();
        if (int_digits == 0 || (int_digits > 1 && text_[int_start] == '0')) {
            return false;
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (digits() == 0) {
                return false;
            }
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
                ++pos_;
            }
            if (digits() == 0) {
                return false;
            }
        }
        out.kind = Value::Kind::Number;
        out.text = std::string(text_.substr(start, pos_ - start));
        return true;
    }

    std::string_view text_;
    std::size_t pos_{0};
};

}  // namespace

std::optional<Value> parse(std::string_view text) { return Parser{text}.document(); }

void append_string(std::string& out, std::string_view text) {
    static constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += "\\u00";
                    out.push_back(kHex[(static_cast<unsigned char>(c) >> 4U) & 0xFU]);
                    out.push_back(kHex[static_cast<unsigned char>(c) & 0xFU]);
                } else {
                    out.push_back(c);   // UTF-8 passes through unchanged
                }
        }
    }
    out.push_back('"');
}

ObjectWriter::ObjectWriter(std::string& out) : out_{out} { out_.push_back('{'); }

void ObjectWriter::key(std::string_view name) {
    if (!first_) {
        out_.push_back(',');
    }
    first_ = false;
    append_string(out_, name);
    out_.push_back(':');
}

void ObjectWriter::string(std::string_view name, std::string_view value) {
    key(name);
    append_string(out_, value);
}

void ObjectWriter::raw(std::string_view name, std::string_view literal) {
    key(name);
    out_ += literal;
}

void ObjectWriter::integer(std::string_view name, std::int64_t value) {
    key(name);
    out_ += std::to_string(value);
}

void ObjectWriter::unsigned_integer(std::string_view name, std::uint64_t value) {
    key(name);
    out_ += std::to_string(value);
}

void ObjectWriter::number(std::string_view name, double value) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument("json: non-finite number cannot be encoded");
    }
    key(name);
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
    out_.append(buffer, result.ptr);
}

void ObjectWriter::close() { out_.push_back('}'); }

}  // namespace trading_engine::events::json
