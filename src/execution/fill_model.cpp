#include "trading_engine/execution/fill_model.hpp"

#include "trading_engine/common/errors.hpp"

namespace trading_engine::execution {

IFillModel::~IFillModel() = default;

std::string_view NaiveFillModel::name() const {
    return "naive";
}

std::vector<domain::Fill> NaiveFillModel::simulate(
    const domain::Order& /*order*/,
    const MarketContext& /*market*/,
    const config::SimulationConfig& /*config*/) const {
    // TODO: fill the full remaining quantity at (reference price +/- slippage
    //       from config.slippage_bps), charge config.fees, set filled_at from
    //       the caller-provided time, and populate slippage/fees on the Fill.
    throw common::NotImplemented("NaiveFillModel::simulate");
}

}  // namespace trading_engine::execution
