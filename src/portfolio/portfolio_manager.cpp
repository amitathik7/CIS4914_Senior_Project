#include "trading_engine/portfolio/portfolio_manager.hpp"

#include "trading_engine/common/errors.hpp"

namespace trading_engine::portfolio {

IPortfolioView::~IPortfolioView() = default;

PortfolioManager::PortfolioManager(common::RunId run_id,
                                   common::Money starting_cash,
                                   const common::IClock& clock)
    : run_id_{run_id}, starting_cash_{starting_cash}, clock_{clock} {}

PortfolioManager::~PortfolioManager() = default;

void PortfolioManager::apply(const domain::Fill& /*fill*/) {
    // TODO: adjust cash by -(qty * price) - fees, update position quantity and
    //       average cost, realise P&L on reducing trades, refresh exposure.
    throw common::NotImplemented("PortfolioManager::apply");
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

}  // namespace trading_engine::portfolio
