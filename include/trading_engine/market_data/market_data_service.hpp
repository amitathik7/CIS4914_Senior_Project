#pragma once

// -----------------------------------------------------------------------------
//  MarketDataService -- validate and normalise raw feed data into MarketEvents.
//
//  Responsibility: the boundary between "whatever the source gave us" and the
//  canonical domain::MarketEvent the rest of the engine trusts. Drops or flags
//  malformed data, stamps ingest time, assigns per-symbol sequence numbers,
//  then forwards to the event bus. Also the tap point for persisting raw
//  market data via IMarketDataRepository.
//
//  Ownership / lifecycle: owned by TradingEngine; created after the event bus,
//  destroyed before it. Holds references (not ownership) to its collaborators.
//
//  Thread-safety: on_market_event() may be invoked from a source I/O thread.
//  The scaffold is single-threaded; the future implementation must document
//  and enforce its threading contract.
// -----------------------------------------------------------------------------

#include "trading_engine/common/clock.hpp"
#include "trading_engine/market_data/market_data_source.hpp"

namespace trading_engine::events { class IEventBus; }
namespace trading_engine::persistence { class IMarketDataRepository; }

namespace trading_engine::market_data {

// Tunables for the validation/normalisation step. Extend as rules are added.
struct MarketDataPolicy {
    bool   reject_non_positive_prices{true};
    bool   reject_out_of_order{false};       // by exchange_time, per symbol
    double max_relative_price_jump{0.0};     // 0 disables the spike filter

    // TODO: stale-quote timeout, crossed-market handling, session-window
    //       filtering, dedupe window, symbol allow-list.
};

class MarketDataService final : public IMarketEventSink {
public:
    // Dependency injection: the service does not create its collaborators.
    // `repository` is optional (nullptr => raw data is not persisted).
    MarketDataService(events::IEventBus& bus,
                      const common::IClock& clock,
                      MarketDataPolicy policy,
                      persistence::IMarketDataRepository* repository = nullptr);
    ~MarketDataService() override;

    // Validate + normalise + (optionally) persist + publish. NOT IMPLEMENTED.
    void on_market_event(const domain::MarketEvent& event) override;

    struct Stats {
        std::uint64_t accepted{0};
        std::uint64_t rejected{0};
        std::uint64_t reordered{0};
    };
    [[nodiscard]] Stats stats() const;   // NOT IMPLEMENTED

private:
    // [[maybe_unused]] only while these are held but not yet consumed by the
    // (unimplemented) processing path; drop the attribute as each is wired in.
    [[maybe_unused]] events::IEventBus&                   bus_;
    [[maybe_unused]] const common::IClock&                clock_;
    [[maybe_unused]] MarketDataPolicy                     policy_;
    [[maybe_unused]] persistence::IMarketDataRepository*  repository_{nullptr};

    // TODO: per-symbol last exchange_time + sequence, spike-filter state,
    //       counters wired to SystemMetrics.
};

}  // namespace trading_engine::market_data
