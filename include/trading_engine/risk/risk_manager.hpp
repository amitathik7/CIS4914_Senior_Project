#pragma once

// -----------------------------------------------------------------------------
//  RiskManager -- gate between strategy signals and order execution.
//
//  Responsibility: for each inbound TradeSignal, read the CURRENT portfolio
//  state, run every configured IRiskPolicy, and produce a single decision:
//  approve as-is, approve resized, or reject (with a reason). Approved signals
//  are forwarded toward the ExecutionSimulator; rejections are recorded.
//
//  Portfolio feedback edge: the RiskManager holds a const reference to an
//  IPortfolioView (implemented by PortfolioManager). This is the required
//  "current portfolio state must be available to the Risk Manager" link.
//  The view returns an immutable snapshot, so no portfolio locks are held
//  during evaluation.
//
//  Ownership / lifecycle: owned by TradingEngine; created after Portfolio
//  Manager, destroyed before it. Policies injected at construction.
//
//  Flow once start()ed: every EventType::Signal on the bus is check()ed;
//  an approved one (with its quantity replaced by adjusted_quantity when a
//  policy resized it) is handed to the approval sink -- in a backtest the
//  ExecutionSimulator's submit() -- and every decision is kept in
//  decisions() and published on the bus as a domain::RiskDecisionReport
//  (EventType::RiskDecision) for out-of-process readers such as the web
//  gateway. MarketData events keep the reference prices and the session
//  start equity current.
//
//  Composition: policies run in order; the first rejection wins; a resize
//  feeds the smaller quantity to the policies after it. When a reservation
//  ledger is attached, a signal every policy approved must also get a hold
//  (ADR 0005 section 4): that is the buying-power and "shares to sell"
//  check, atomic against other signals queued in the same bar.
//
//  Kill switch (plan M6): while engaged, every signal is rejected before any
//  policy runs, with reason "kill_switch" and the operator's text as the
//  detail. Orders already working are not cancelled (the simulator has no
//  cancels yet). set_kill_switch() may be called from any thread, for example
//  by the command channel (docs/adr/0007-engine-command-channel.md).
//
//  Thread-safety: handlers run on the bus dispatcher; public methods take one
//  mutex, so decisions() and stats() may be read from any thread.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <memory>
#include <vector>

#include "trading_engine/common/clock.hpp"
#include "trading_engine/common/identifiers.hpp"
#include "trading_engine/domain/market_event.hpp"
#include "trading_engine/configuration/engine_config.hpp"
#include "trading_engine/domain/trade_signal.hpp"
#include "trading_engine/risk/risk_policy.hpp"

namespace trading_engine::events { class IEventBus; }
namespace trading_engine::portfolio { class IPortfolioView; class IReservationLedger; }

namespace trading_engine::risk {

struct RiskRecord {
    domain::TradeSignal signal{};     // as the strategy sent it
    RiskDecision        decision{};
    std::string         policy{};     // the deciding policy; empty when approved unchanged
};

class RiskManager {
public:
    struct Stats {
        std::uint64_t evaluated{0};
        std::uint64_t approved{0};    // including resized
        std::uint64_t resized{0};
        std::uint64_t rejected{0};
    };
    using ApprovalSink = std::function<void(const domain::TradeSignal&)>;

    struct KillSwitch {
        bool                             engaged{false};
        std::optional<common::Timestamp> changed_at{};   // unset until first changed
        std::string                      reason{};       // empty when released
    };

    RiskManager(const portfolio::IPortfolioView& portfolio,
                events::IEventBus& bus,
                const common::IClock& clock,
                config::RiskLimits limits,
                std::vector<std::shared_ptr<IRiskPolicy>> policies);
    ~RiskManager();

    RiskManager(const RiskManager&)            = delete;
    RiskManager& operator=(const RiskManager&) = delete;

    // Before start(). The ledger and sink are borrowed / copied.
    void set_reservation_ledger(portfolio::IReservationLedger* ledger) noexcept { ledger_ = ledger; }
    void set_approval_sink(ApprovalSink sink) { sink_ = std::move(sink); }
    // Stamped on every published RiskDecisionReport (default 1).
    void set_run_id(common::RunId run_id) noexcept { run_id_ = run_id; }

    // Subscribe to Signal and MarketData. Idempotent. stop() unsubscribes.
    void start();
    void stop();

    // Evaluate one signal against all policies using a fresh portfolio
    // snapshot (and place the hold, when a ledger is attached). Records it.
    [[nodiscard]] RiskDecision check(const domain::TradeSignal& signal);

    // Update the reference price and session for event.symbol.
    void on_market_event(const domain::MarketEvent& event);

    void add_policy(std::shared_ptr<IRiskPolicy> policy);

    // Engage (reason is shown on every rejection) or release. Stamped with the
    // clock's now(). Returns the new state. Any thread.
    KillSwitch set_kill_switch(bool engaged, std::string reason = {});
    [[nodiscard]] KillSwitch kill_switch() const;

    [[nodiscard]] std::size_t policy_count() const noexcept { return policies_.size(); }
    [[nodiscard]] std::vector<RiskRecord> decisions() const;
    [[nodiscard]] Stats stats() const;

private:
    const portfolio::IPortfolioView&           portfolio_;
    events::IEventBus&                         bus_;
    const common::IClock&                      clock_;
    config::RiskLimits                         limits_;
    std::vector<std::shared_ptr<IRiskPolicy>>  policies_{};
    portfolio::IReservationLedger*             ledger_{nullptr};
    ApprovalSink                               sink_{};
    common::RunId                              run_id_{1};

    mutable std::mutex                         mutex_;
    std::vector<common::SubscriptionId>        subscriptions_{};
    std::map<common::Symbol, common::Price>    prices_{};
    std::int64_t                               session_day_{-1};
    std::optional<common::Money>               session_start_equity_{};
    std::vector<RiskRecord>                    decisions_{};
    Stats                                      stats_{};
    std::uint64_t                              reports_issued_{0};
    KillSwitch                                 kill_switch_{};

    // TODO: rejection log wired to persistence,
    //       per-symbol / per-strategy limit overrides.
};

}  // namespace trading_engine::risk
