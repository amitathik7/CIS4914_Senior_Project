#include "trading_engine/portfolio/portfolio_manager.hpp"

#include "trading_engine/common/errors.hpp"

namespace trading_engine::portfolio {

IPortfolioView::~IPortfolioView() = default;
IReservationLedger::~IReservationLedger() = default;

PortfolioManager::PortfolioManager(common::RunId run_id,
                                   common::Money starting_cash,
                                   const common::IClock& clock)
    : run_id_{run_id}, starting_cash_{starting_cash}, clock_{clock} {}

PortfolioManager::~PortfolioManager() = default;

bool PortfolioManager::apply(const domain::Fill& /*fill*/) {
    // TODO: reject already-seen Fill::id (idempotency, see ADR 0004), then
    //       adjust cash by -(qty * price) - fees, update position quantity and
    //       average cost, realise P&L on reducing trades, refresh exposure.
    throw common::NotImplemented("PortfolioManager::apply");
}

bool PortfolioManager::apply_order_update(const domain::Order& /*order*/) {
    // TODO: per ADR 0004 section 7 -- reserve on Working, release on
    //       Rejected / Cancelled / Expired, ignore fill-driven statuses, and
    //       stay idempotent by state keyed on Order::id.
    throw common::NotImplemented("PortfolioManager::apply_order_update");
}

void PortfolioManager::mark(const domain::MarketEvent& /*event*/) {
    // TODO: update last mark price for the symbol and recompute unrealised P&L
    //       and total equity.
    throw common::NotImplemented("PortfolioManager::mark");
}

domain::PortfolioSnapshot PortfolioManager::snapshot() const {
    // Must not hand back a zero/starting-cash snapshot as if it were real
    // state -- see docs (no fabricated portfolio data).
    throw common::NotImplemented("PortfolioManager::snapshot");
}

std::optional<Position> PortfolioManager::position(
    const common::Symbol& /*symbol*/) const {
    throw common::NotImplemented("PortfolioManager::position");
}

common::Money PortfolioManager::cash() const {
    throw common::NotImplemented("PortfolioManager::cash");
}

ReservationResult PortfolioManager::hold_for_signal(
    common::SignalId /*signal*/,
    const common::Symbol& /*symbol*/,
    domain::OrderSide /*side*/,
    common::Quantity /*quantity*/,
    std::optional<common::Price> /*limit_price*/) {
    // TODO: size the hold (limit: qty * limit_price + fees; market: last
    //       mark * buffer), compare against buying power and hold in ONE
    //       critical section. Granting without the check, or checking
    //       without holding, reopens the window this exists to close.
    throw common::NotImplemented("PortfolioManager::hold_for_signal");
}

void PortfolioManager::release_signal_hold(common::SignalId /*signal*/) {
    throw common::NotImplemented("PortfolioManager::release_signal_hold");
}

common::Money PortfolioManager::buying_power() const {
    // Must not return cash() or starting cash as if nothing were reserved --
    // that would be fabricated state (see scaffold_contract_test).
    throw common::NotImplemented("PortfolioManager::buying_power");
}

}  // namespace trading_engine::portfolio
