#pragma once

// -----------------------------------------------------------------------------
//  PortfolioBusBridge -- connects a PortfolioManager to the event bus.
//
//  PortfolioManager itself knows nothing about the bus (it is a plain,
//  mutex-protected state holder; see portfolio_manager.hpp). This bridge
//  subscribes on its behalf and forwards:
//    EventType::MarketData -> PortfolioManager::mark()
//    EventType::Order      -> PortfolioManager::apply_order_update()
//    EventType::Fill       -> PortfolioManager::apply()
//  per the flow in docs/DATA_FLOW.md and ADR 0004.
//
//  A fill the portfolio refuses (duplicate id or another run: apply() returns
//  false) is counted as ignored. One that breaks the contract (apply() throws
//  common::ValidationError) is counted as an error and its message kept in
//  last_error(); the bridge never lets it escape into the bus.
//
//  Lifecycle: attach() subscribes, detach() unsubscribes (a barrier, so no
//  handler is running once it returns). The destructor detaches. Both bus and
//  portfolio are borrowed and must outlive the bridge.
//
//  Thread-safety: handlers run on the bus dispatcher; counters are atomic and
//  last_error() is locked, so they may be read from any thread.
// -----------------------------------------------------------------------------

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "trading_engine/common/identifiers.hpp"

namespace trading_engine::events { class IEventBus; }

namespace trading_engine::portfolio {

class PortfolioManager;

class PortfolioBusBridge {
public:
    struct Stats {
        std::uint64_t marks{0};           // market events forwarded to mark()
        std::uint64_t order_updates{0};   // apply_order_update() calls
        std::uint64_t fills_applied{0};   // apply() returned true
        std::uint64_t fills_ignored{0};   // apply() returned false (duplicate / other run)
        std::uint64_t errors{0};          // apply() / apply_order_update() threw
    };

    PortfolioBusBridge(events::IEventBus& bus, PortfolioManager& portfolio);
    ~PortfolioBusBridge();

    PortfolioBusBridge(const PortfolioBusBridge&)            = delete;
    PortfolioBusBridge& operator=(const PortfolioBusBridge&) = delete;

    void attach();            // no-op if attached
    void detach() noexcept;   // no-op if detached

    [[nodiscard]] Stats stats() const noexcept;
    [[nodiscard]] std::string last_error() const;

private:
    void record_error(const char* what);

    events::IEventBus& bus_;
    PortfolioManager&  portfolio_;
    std::vector<common::SubscriptionId> subscriptions_{};

    std::atomic<std::uint64_t> marks_{0};
    std::atomic<std::uint64_t> order_updates_{0};
    std::atomic<std::uint64_t> fills_applied_{0};
    std::atomic<std::uint64_t> fills_ignored_{0};
    std::atomic<std::uint64_t> errors_{0};

    mutable std::mutex error_mutex_;
    std::string last_error_{};
};

}  // namespace trading_engine::portfolio
