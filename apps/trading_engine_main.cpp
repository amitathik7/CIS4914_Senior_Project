// Thin entry point for the scaffold.
//
// Its only job is to prove the project compiles and links. It does NOT
// construct or start the engine -- there is nothing to run yet. It must not
// imply the system is operational.

#include <iostream>

#include "trading_engine/domain/order.hpp"
#include "trading_engine/version.hpp"

int main() {
    using namespace trading_engine;  // NOLINT(build/namespaces) -- tiny main only

    std::cout << kProjectName << ' ' << kVersion << " -- scaffold build OK\n"
              << kDescription << "\n\n"
              << "Nothing is wired up yet: no market data, strategies, risk\n"
              << "checks, order execution, persistence, or analytics. This\n"
              << "binary only confirms the scaffold compiles and links.\n"
              << "Next steps: docs/IMPLEMENTATION_PLAN.md\n\n"
              << "  link check: domain::to_string(OrderSide::Buy) = \""
              << domain::to_string(domain::OrderSide::Buy) << "\"\n"
              << "  scaffold flag: " << std::boolalpha << kIsScaffold << '\n';
    return 0;
}
