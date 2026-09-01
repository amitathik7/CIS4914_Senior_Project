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
//  Thread-safety: evaluates on one bus-worker thread in the scaffold. The
//  IPortfolioView implementation is responsible for making snapshot() safe to
//  call from this thread.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <memory>
#include <vector>

#include "trading_engine/common/clock.hpp"
#include "trading_engine/configuration/engine_config.hpp"
#include "trading_engine/domain/trade_signal.hpp"
#include "trading_engine/risk/risk_policy.hpp"

namespace trading_engine::events { class IEventBus; }
namespace trading_engine::portfolio { class IPortfolioView; }

namespace trading_engine::risk {

class RiskManager {
public:
    RiskManager(const portfolio::IPortfolioView& portfolio,
                events::IEventBus& bus,
                const common::IClock& clock,
                config::RiskLimits limits,
                std::vector<std::shared_ptr<IRiskPolicy>> policies);
    ~RiskManager();

    void start();   // NOT IMPLEMENTED: subscribe to EventType::Signal
    void stop();    // NOT IMPLEMENTED

    // Evaluate one signal against all policies using a fresh portfolio
    // snapshot. NOT IMPLEMENTED (throws common::NotImplemented).
    [[nodiscard]] RiskDecision check(const domain::TradeSignal& signal);

    void add_policy(std::shared_ptr<IRiskPolicy> policy);

    [[nodiscard]] std::size_t policy_count() const noexcept { return policies_.size(); }

private:
    [[maybe_unused]] const portfolio::IPortfolioView&  portfolio_;
    [[maybe_unused]] events::IEventBus&                bus_;
    [[maybe_unused]] const common::IClock&             clock_;
    [[maybe_unused]] config::RiskLimits                limits_;
    std::vector<std::shared_ptr<IRiskPolicy>>          policies_{};

    // TODO: open-order accounting, realised-P&L-today tracking and reset at
    //       session boundaries, first-fail vs collect-all-reasons policy,
    //       rejection log wired to persistence, kill-switch latch.
};

}  // namespace trading_engine::risk
