#pragma once

// -----------------------------------------------------------------------------
//  Common exception types.
//
//  Responsibility: make the scaffold's "not built yet" state explicit and
//  greppable. A stub that is called at runtime must throw NotImplemented -- it
//  must never silently return fabricated data.
// -----------------------------------------------------------------------------

#include <stdexcept>
#include <string>
#include <string_view>

namespace trading_engine::common {

// Thrown by scaffold stubs that have a signature but no behaviour yet.
class NotImplemented : public std::logic_error {
public:
    explicit NotImplemented(std::string_view what)
        : std::logic_error("not implemented yet: " + std::string{what}) {}
};

// Thrown when configuration is missing, malformed, or inconsistent.
class ConfigError : public std::runtime_error {
public:
    explicit ConfigError(std::string_view what)
        : std::runtime_error("configuration error: " + std::string{what}) {}
};

// Thrown when inbound data fails validation at a system boundary
// (e.g. a malformed market-data payload).
class ValidationError : public std::runtime_error {
public:
    explicit ValidationError(std::string_view what)
        : std::runtime_error("validation error: " + std::string{what}) {}
};

}  // namespace trading_engine::common
