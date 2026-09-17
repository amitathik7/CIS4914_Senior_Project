# Fast unit tests -- no network, no database, no threads.

# Proves the GoogleTest target builds, links against the core library, and runs.
trading_engine_add_component_test(smoke
    smoke_test.cpp)

# Exercises the small pieces of real logic the scaffold does ship.
trading_engine_add_component_test(common
    common_identifiers_test.cpp
    common_clock_test.cpp)

# Shape-only checks for the ADR 0002 (Proposed, not Accepted) TradeSignal
# additions and the new SignalCancelRequest type. Not validation -- neither
# enforces invariants yet; see docs/adr/0002-strategy-risk-signal-contract.md.
trading_engine_add_component_test(domain
    domain_trade_signal_test.cpp)

# Pins the scaffold contract: unimplemented stubs throw NotImplemented, they do
# not return fabricated data.
trading_engine_add_component_test(scaffold_contract
    scaffold_contract_test.cpp)

# TODO(team): add one call per component as behaviour lands, e.g.
#   trading_engine_add_component_test(market_data market_data_service_test.cpp)
#   trading_engine_add_component_test(strategy    strategy_engine_test.cpp)
#   trading_engine_add_component_test(risk        risk_manager_test.cpp)
#   trading_engine_add_component_test(execution   execution_simulator_test.cpp)
#   trading_engine_add_component_test(portfolio   portfolio_manager_test.cpp)
#   trading_engine_add_component_test(analytics   performance_analyzer_test.cpp)
