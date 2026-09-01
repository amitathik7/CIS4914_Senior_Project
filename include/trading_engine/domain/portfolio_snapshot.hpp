#pragma once

// -----------------------------------------------------------------------------
//  PortfolioSnapshot -- an immutable view of account state at one instant.
//
//  Responsibility: the value the RiskManager reads to evaluate a signal
//  against current exposure, and the record the persistence layer stores for
//  later analytics. Produced by PortfolioManager::snapshot().
//
//  Plain value type. A snapshot is a copy: safe to hand across threads.
// -----------------------------------------------------------------------------

#include <vector>

#include "trading_engine/common/identifiers.hpp"
#include "trading_engine/common/types.hpp"
#include "trading_engine/portfolio/position.hpp"

namespace trading_engine::domain {

struct PortfolioSnapshot {
    common::RunId     run_id{};
    common::Timestamp as_of{};        // UTC

    common::Money cash{0};            // free cash in account currency
    common::Money total_equity{0};    // cash + marked value of positions

    std::vector<portfolio::Position> positions{};

    common::Money gross_exposure{0};  // sum of |position notional|
    common::Money net_exposure{0};    // signed sum of position notional

    // TODO: buying power / margin used, per-strategy sub-accounts, currency
    //       breakdown, realised vs unrealised split, and a monotonically
    //       increasing revision number for ordering snapshots.
};

}  // namespace trading_engine::domain
