#include "trading_engine/risk/risk_manager.hpp"

#include <utility>

#include "trading_engine/common/errors.hpp"

namespace trading_engine::risk {

RiskManager::RiskManager(const portfolio::IPortfolioView& portfolio,
                         events::IEventBus& bus,
                         const common::IClock& clock,
                         config::RiskLimits limits,
                         std::vector<std::shared_ptr<IRiskPolicy>> policies)
    : portfolio_{portfolio},
      bus_{bus},
      clock_{clock},
      limits_{limits},
      policies_{std::move(policies)} {}

RiskManager::~RiskManager() = default;

void RiskManager::start() {
    // TODO: subscribe to EventType::Signal on bus_.
    throw common::NotImplemented("RiskManager::start");
}

void RiskManager::stop() {
    throw common::NotImplemented("RiskManager::stop");
}

RiskDecision RiskManager::check(const domain::TradeSignal& /*signal*/) {
    // TODO: take a fresh portfolio_.snapshot(), build a RiskContext from
    //       limits_ + open-order/realised-PnL tracking, run every policy in
    //       policies_, and combine results (first-fail vs collect-all). Publish
    //       approved signals; record rejections.
    throw common::NotImplemented("RiskManager::check");
}

void RiskManager::add_policy(std::shared_ptr<IRiskPolicy> policy) {
    if (policy == nullptr) {
        throw common::ValidationError(
            "RiskManager::add_policy: policy must not be null");
    }
    policies_.push_back(std::move(policy));
}

}  // namespace trading_engine::risk
