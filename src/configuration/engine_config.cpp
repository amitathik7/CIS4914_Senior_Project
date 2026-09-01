#include "trading_engine/configuration/engine_config.hpp"

#include "trading_engine/common/errors.hpp"

namespace trading_engine::config {

EngineConfig default_config() {
    // Every default lives in the struct member initialisers in the header, so
    // this is simply a value-initialised aggregate. Returning concrete
    // configuration (not market/portfolio/performance data) is intentional and
    // keeps tests and examples runnable without a config file.
    return EngineConfig{};
}

EngineConfig load_config(const std::filesystem::path& /*file*/) {
    // TODO: choose a config format + parser (see docs/OPEN_QUESTIONS.md), then
    //       parse, apply environment-variable overrides, and validate().
    throw common::NotImplemented(
        "config::load_config -- no config file format/parser chosen yet");
}

void validate(const EngineConfig& /*config*/) {
    // TODO: positive starting cash, non-empty URLs, risk limits ordered
    //       sensibly, pool_size > 0, symbols non-empty in live mode, etc.
    throw common::NotImplemented("config::validate");
}

}  // namespace trading_engine::config
