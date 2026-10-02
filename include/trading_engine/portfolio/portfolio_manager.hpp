#pragma once

// -----------------------------------------------------------------------------
//  PortfolioManager -- single source of truth for cash, positions and P&L.
//
//  Responsibility: consume Fills to update cash, position quantity and cost
//  basis, and realised P&L; consume Order lifecycle events to reserve cash at
//  submission and release it when an order is rejected, cancelled or expires;
//  consume MarketEvents to re-mark open positions for unrealised P&L; expose an
//  immutable PortfolioSnapshot for the RiskManager, analytics, and persistence.
//
//  Functions as a traditional portfolio manager: it owns open-order state and
//  buying power (cash minus reserved), per the team decision recorded in
//  docs/adr/0004-execution-portfolio-fill-contract.md sections 7-8.
//
//  IPortfolioView is the read side, deliberately narrow, so consumers (notably
//  RiskManager) depend only on "give me the current snapshot", not on the
//  mutating API. IReservationLedger is the one exception: the RiskManager's
//  atomic check-and-hold, kept on its own interface so the read seam stays
//  read-only.
//
//  Ownership / lifecycle: owned by TradingEngine; one of the first components
//  created and the last destroyed, since others hold IPortfolioView& to it.
//
//  Thread-safety: writes (apply/apply_order_update/mark) arrive on the
//  execution/bus thread; reads come from the RiskManager thread. Every public
//  method takes one mutex, so reads never see a half-applied write.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "trading_engine/common/clock.hpp"
#include "trading_engine/common/identifiers.hpp"
#include "trading_engine/common/types.hpp"
#include "trading_engine/configuration/engine_config.hpp"
#include "trading_engine/domain/fill.hpp"
#include "trading_engine/domain/market_event.hpp"
#include "trading_engine/domain/order.hpp"
#include "trading_engine/domain/portfolio_snapshot.hpp"
#include "trading_engine/portfolio/pending_order.hpp"
#include "trading_engine/portfolio/position.hpp"
#include "trading_engine/portfolio/reservation_ledger.hpp"

namespace trading_engine::portfolio {

// Read-only view of portfolio state. Implemented by PortfolioManager.
class IPortfolioView {
public:
    virtual ~IPortfolioView();

    [[nodiscard]] virtual domain::PortfolioSnapshot snapshot() const = 0;
    [[nodiscard]] virtual std::optional<Position> position(
        const common::Symbol& symbol) const = 0;

    // Settled cash. Moves only when fills settle.
    [[nodiscard]] virtual common::Money cash() const = 0;

    // cash() minus cash reserved against open buy orders: what is actually
    // free to commit to a new buy. PROPOSED by
    // docs/adr/0005-portfolio-risk-query-contract.md sections 4 and 6.
    [[nodiscard]] virtual common::Money buying_power() const = 0;
};

class PortfolioManager final : public IPortfolioView,
                              public IReservationLedger {
public:
    // A market buy has no price, so it is held at the last mark plus
    // market_buy_buffer (0.05 = 5%) to cover the price moving before it fills.
    PortfolioManager(common::RunId run_id,
                     common::Money starting_cash,
                     const common::IClock& clock,
                     config::FeeModelConfig fees = {},
                     double market_buy_buffer = 0.05);
    ~PortfolioManager() override;

    // --- write side (mutating) ---------------------------------------
    // Two entry points for execution results, per
    // docs/adr/0004-execution-portfolio-fill-contract.md section 13.

    // Apply a simulated execution: cash moves by -(qty * price) - fees, the
    // position's quantity, average cost and realised P&L update, the symbol
    // is marked at the fill price, and the order's reservation shrinks by the
    // filled share. A fill with status Filled releases whatever is left.
    //
    // Returns false, changing nothing, for an already-applied Fill::id or a
    // fill from another run. Throws common::ValidationError for a fill that
    // breaks the contract (zero quantity, non-positive price, negative fees,
    // a gap in Fill::sequence, or a fill for an order already closed).
    bool apply(const domain::Fill& fill);

    // Apply an order lifecycle event. Working: open the order, taking over
    // its signal's hold (or reserving now if there is none). Rejected,
    // Cancelled, Expired: release what the order or its signal still holds.
    // Other statuses are ignored, because fills drive them. Returns true if
    // open orders or reservations changed.
    bool apply_order_update(const domain::Order& order);

    // Record the latest price for a symbol: the last trade price, else the
    // bid/ask midpoint. Events with no positive price are ignored.
    void mark(const domain::MarketEvent& event);

    // --- read side (IPortfolioView) --------------------------------
    [[nodiscard]] domain::PortfolioSnapshot snapshot() const override;
    [[nodiscard]] std::optional<Position> position(
        const common::Symbol& symbol) const override;
    [[nodiscard]] common::Money cash() const override;
    [[nodiscard]] common::Money buying_power() const override;

    // --- reservation side (IReservationLedger) ---------------------
    [[nodiscard]] ReservationResult hold_for_signal(
        common::SignalId signal,
        const common::Symbol& symbol,
        domain::OrderSide side,
        common::Quantity quantity,
        std::optional<common::Price> limit_price) override;
    void release_signal_hold(common::SignalId signal) override;

private:
    struct Hold {
        common::Symbol    symbol;
        domain::OrderSide side;
        common::Quantity  quantity;
        common::Money     amount;
    };

    // Callers of everything below must hold mutex_.

    // A position is valued at its last mark, or at average cost if the
    // symbol has never been marked.
    [[nodiscard]] common::Price mark_price(const Position& position) const;
    [[nodiscard]] Position valued(const Position& position) const;

    // Cash to hold for a buy; nullopt for a market buy with no mark yet.
    [[nodiscard]] std::optional<common::Money> buy_cost(
        const common::Symbol& symbol, common::Quantity quantity,
        std::optional<common::Price> limit_price) const;
    [[nodiscard]] common::Money reserved_total() const;
    // Shares already promised to sell holds and open sell orders.
    [[nodiscard]] common::Quantity committed_to_sell(const common::Symbol& symbol) const;
    void release_for_fill(const domain::Fill& fill);

    const common::RunId          run_id_;
    const common::IClock&        clock_;
    const config::FeeModelConfig fees_;
    const double                 market_buy_buffer_;

    mutable std::mutex mutex_;

    common::Money cash_;

    // std::map so snapshots list entries in a stable order.
    std::map<common::Symbol, Position>      positions_{};
    std::map<common::Symbol, common::Price> marks_{};
    std::map<common::OrderId, PendingOrder> open_orders_{};

    std::unordered_map<common::SignalId, Hold> holds_{};
    // Orders that were filled, rejected, cancelled or expired, so a late
    // Working event cannot reserve for them again.
    std::unordered_set<common::OrderId> closed_orders_{};

    std::unordered_set<common::FillId> applied_fills_{};
    // Last Fill::sequence seen per order still filling, for gap detection.
    std::unordered_map<common::OrderId, std::uint32_t> last_sequence_{};
};

}  // namespace trading_engine::portfolio
