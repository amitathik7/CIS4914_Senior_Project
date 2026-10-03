#include "lab/json_writer.hpp"

#include <charconv>
#include <cmath>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "lab/utf8.hpp"
#include "trading_engine/common/decimal_text.hpp"

namespace trading_engine::lab {

void JsonWriter::newline(std::size_t depth) {
    if (!pretty_) {
        return;
    }
    out_ += '\n';
    out_.append((base_depth_ + depth) * 2, ' ');
}

void JsonWriter::before_value() {
    if (stack_.empty()) {
        if (root_done_) {
            throw std::logic_error("JsonWriter: a document has exactly one root value");
        }
        root_done_ = true;
        return;
    }
    Frame& frame = stack_.back();
    if (frame.is_object) {
        if (!frame.have_key) {
            throw std::logic_error("JsonWriter: a value inside an object needs a key first");
        }
        frame.have_key = false;   // the comma and indentation were written with the key
        return;
    }
    if (!frame.first) {
        out_ += ',';
    }
    frame.first = false;
    newline(stack_.size());
}

JsonWriter& JsonWriter::begin_object() {
    before_value();
    out_ += '{';
    stack_.push_back(Frame{true, true, false});
    return *this;
}

JsonWriter& JsonWriter::end_object() {
    if (stack_.empty() || !stack_.back().is_object || stack_.back().have_key) {
        throw std::logic_error("JsonWriter: end_object() does not close an open object");
    }
    const bool empty = stack_.back().first;
    stack_.pop_back();
    if (!empty) {
        newline(stack_.size());
    }
    out_ += '}';
    return *this;
}

JsonWriter& JsonWriter::begin_array() {
    before_value();
    out_ += '[';
    stack_.push_back(Frame{false, true, false});
    return *this;
}

JsonWriter& JsonWriter::end_array() {
    if (stack_.empty() || stack_.back().is_object) {
        throw std::logic_error("JsonWriter: end_array() does not close an open array");
    }
    const bool empty = stack_.back().first;
    stack_.pop_back();
    if (!empty) {
        newline(stack_.size());
    }
    out_ += ']';
    return *this;
}

JsonWriter& JsonWriter::key(std::string_view name) {
    if (stack_.empty() || !stack_.back().is_object || stack_.back().have_key) {
        throw std::logic_error("JsonWriter: a key is only valid directly inside an object");
    }
    Frame& frame = stack_.back();
    if (!frame.first) {
        out_ += ',';
    }
    frame.first = false;
    newline(stack_.size());
    write_string(name);
    out_ += pretty_ ? ": " : ":";
    frame.have_key = true;
    return *this;
}

void JsonWriter::write_string(std::string_view value) {
    if (!is_valid_utf8(value)) {
        throw std::invalid_argument("JsonWriter: string is not valid UTF-8");
    }
    constexpr std::string_view kHex = "0123456789abcdef";
    out_ += '"';
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        switch (c) {
            case '"':  out_ += "\\\""; continue;
            case '\\': out_ += "\\\\"; continue;
            case '\b': out_ += "\\b"; continue;
            case '\f': out_ += "\\f"; continue;
            case '\n': out_ += "\\n"; continue;
            case '\r': out_ += "\\r"; continue;
            case '\t': out_ += "\\t"; continue;
            default: break;
        }
        if (c < 0x20U || c == 0x7FU) {
            out_ += "\\u00";
            out_ += kHex[(c >> 4U) & 0xFU];
            out_ += kHex[c & 0xFU];
        } else if (c == 0xE2U && i + 2 < value.size() && static_cast<unsigned char>(value[i + 1]) == 0x80U &&
                   (static_cast<unsigned char>(value[i + 2]) == 0xA8U ||
                    static_cast<unsigned char>(value[i + 2]) == 0xA9U)) {
            // U+2028 / U+2029 are legal JSON but break JavaScript string literals.
            out_ += static_cast<unsigned char>(value[i + 2]) == 0xA8U ? "\\u2028" : "\\u2029";
            i += 2;
        } else {
            out_ += static_cast<char>(c);
        }
    }
    out_ += '"';
}

JsonWriter& JsonWriter::string(std::string_view value) {
    before_value();
    write_string(value);
    return *this;
}

JsonWriter& JsonWriter::boolean(bool value) {
    before_value();
    out_ += value ? "true" : "false";
    return *this;
}

JsonWriter& JsonWriter::null() {
    before_value();
    out_ += "null";
    return *this;
}

JsonWriter& JsonWriter::integer(std::int64_t value) {
    before_value();
    char buffer[32];
    const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value);
    out_.append(buffer, result.ptr);
    return *this;
}

JsonWriter& JsonWriter::unsigned_integer(std::uint64_t value) {
    before_value();
    char buffer[32];
    const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value);
    out_.append(buffer, result.ptr);
    return *this;
}

JsonWriter& JsonWriter::number(double value) {
    if (!std::isfinite(value)) {
        throw std::domain_error("JsonWriter: JSON cannot represent NaN or infinity");
    }
    before_value();
    char buffer[64];   // the longest shortest-round-trip double is 24 characters
    const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value);
    if (result.ec != std::errc{}) {
        throw std::logic_error("JsonWriter: could not format a double");
    }
    out_.append(buffer, result.ptr);
    return *this;
}

JsonWriter& JsonWriter::scaled_number(std::int64_t scaled, int decimals) {
    before_value();
    out_ += common::format_scaled(scaled, decimals);
    return *this;
}

const std::string& JsonWriter::str() const {
    if (!complete()) {
        throw std::logic_error("JsonWriter: the document is not complete");
    }
    return out_;
}

std::string JsonWriter::take() {
    if (!complete()) {
        throw std::logic_error("JsonWriter: the document is not complete");
    }
    return std::move(out_);
}

}  // namespace trading_engine::lab
