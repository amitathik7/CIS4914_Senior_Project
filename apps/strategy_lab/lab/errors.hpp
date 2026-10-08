#pragma once

// -----------------------------------------------------------------------------
//  Errors of the Strategy Lab tool, and the process exit status each maps to.
//
//  Exit status of strategy_lab_replay (documented in docs/STRATEGY_LAB.md):
//     0  the document on stdout is a result (describe, or a finished replay)
//     2  usage or configuration: bad arguments, an unknown or unparsable parameter,
//        an unavailable strategy, or a strategy configuration the strategy itself
//        rejected (its message is passed through verbatim)
//     3  dataset: unreadable, malformed, out of order, or over the row limit
//     4  internal: a bug, or stdout could not be written
//  In every case stdout holds exactly ONE JSON document (the result, or a
//  strategy_lab.error document) and stderr holds human-readable text.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trading_engine::lab {

enum class ErrorCode {
    UsageError,
    InvalidParameter,
    UnavailableStrategy,
    ConfigError,
    DatasetError,
    LimitExceeded,
    InternalError
};

inline constexpr int kExitOk       = 0;
inline constexpr int kExitUsage    = 2;
inline constexpr int kExitDataset  = 3;
inline constexpr int kExitInternal = 4;

[[nodiscard]] constexpr std::string_view code_name(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::UsageError:          return "usage_error";
        case ErrorCode::InvalidParameter:    return "invalid_parameter";
        case ErrorCode::UnavailableStrategy: return "unavailable_strategy";
        case ErrorCode::ConfigError:         return "config_error";
        case ErrorCode::DatasetError:        return "dataset_error";
        case ErrorCode::LimitExceeded:       return "limit_exceeded";
        case ErrorCode::InternalError:       return "internal_error";
    }
    return "internal_error";
}

[[nodiscard]] constexpr int exit_status(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::UsageError:
        case ErrorCode::InvalidParameter:
        case ErrorCode::UnavailableStrategy:
        case ErrorCode::ConfigError:         return kExitUsage;
        case ErrorCode::DatasetError:
        case ErrorCode::LimitExceeded:       return kExitDataset;
        case ErrorCode::InternalError:       return kExitInternal;
    }
    return kExitInternal;
}

// One specific thing that is wrong. `line` is a 1-based CSV line (0 = not about a
// line); `column` names a CSV column or a parameter; `where` locates a
// configuration item such as "strategies[1]".
struct Problem {
    std::size_t line{0};
    std::string column{};
    std::string where{};
    std::string message{};
};

class LabError : public std::runtime_error {
public:
    LabError(ErrorCode code, std::string message, std::vector<Problem> problems = {},
             std::size_t total_problems = 0)
        : std::runtime_error{std::move(message)},
          code_{code},
          problems_{std::move(problems)},
          total_problems_{total_problems > problems_.size() ? total_problems : problems_.size()} {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] const std::vector<Problem>& problems() const noexcept { return problems_; }
    // How many problems there were (more than problems().size() when the list is capped).
    [[nodiscard]] std::size_t total_problems() const noexcept { return total_problems_; }

private:
    ErrorCode            code_;
    std::vector<Problem> problems_;
    std::size_t          total_problems_;
};

}  // namespace trading_engine::lab
