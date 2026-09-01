#pragma once

// -----------------------------------------------------------------------------
//  IFillModel -- interface for how a resting/marketable order becomes fills.
//
//  Responsibility: encapsulate the execution assumptions of the simulation --
//  latency, slippage, fees, and partial-fill behaviour -- so they can be
//  swapped (naive, spread-crossing, volume-participation, stochastic) without
//  touching the ExecutionSimulator.
//
//  Contract: simulate() is a pure function of (order, market context, config).
//  It returns zero or more Fills whose signed quantities sum to at most the
//  order's remaining quantity. No global state, safe to call concurrently.
// -----------------------------------------------------------------------------

#include <optional>
#include <string_view>
#include <vector>

#include "trading_engine/common/types.hpp"
#include "trading_engine/configuration/engine_config.hpp"
#include "trading_engine/domain/fill.hpp"
#include "trading_engine/domain/order.hpp"

namespace trading_engine::execution {

// The slice of market state the model is allowed to see. Kept deliberately
// small; grow it as models get more sophisticated.
struct MarketContext {
    common::Symbol    symbol{};
    common::Timestamp as_of{};
    std::optional<common::Price>    last_price{};
    std::optional<common::Price>    bid{};
    std::optional<common::Price>    ask{};
    std::optional<common::Quantity> recent_volume{};
    // TODO: order-book depth, volatility estimate, time-since-last-trade,
    //       session phase (auction/continuous/closed).
};

class IFillModel {
public:
    virtual ~IFillModel();

    [[nodiscard]] virtual std::string_view name() const = 0;

    [[nodiscard]] virtual std::vector<domain::Fill> simulate(
        const domain::Order& order,
        const MarketContext& market,
        const config::SimulationConfig& config) const = 0;
};

// Simplest possible model: fill the whole order at a reference price adjusted
// by a fixed slippage in basis points, charge configured fees, zero latency.
// Declared here, DEFINED as a throwing stub for now.
class NaiveFillModel final : public IFillModel {
public:
    [[nodiscard]] std::string_view name() const override;
    [[nodiscard]] std::vector<domain::Fill> simulate(
        const domain::Order& order,
        const MarketContext& market,
        const config::SimulationConfig& config) const override;   // NOT IMPLEMENTED
};

}  // namespace trading_engine::execution
