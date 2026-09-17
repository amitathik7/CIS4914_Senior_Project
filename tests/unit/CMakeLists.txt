# Fast unit tests -- no network, no database, no threads.

# Proves the GoogleTest target builds, links against the core library, and runs.
trading_engine_add_component_test(smoke
    smoke_test.cpp)

# Exercises the small pieces of real logic the scaffold does ship.
trading_engine_add_component_test(common
    common_identifiers_test.cpp
    common_clock_test.cpp)

# Shape-only checks for the ADR 0004 (Proposed, not Accepted) Fill/Order
# additions. Not validation -- neither type enforces invariants yet; see
# docs/adr/0004-execution-portfolio-fill-contract.md.
#
# MERGE NOTE -- conflicts with the ADR 0002 branch (issue #3), which adds its
# own `trading_engine_add_component_test(domain domain_trade_signal_test.cpp)`.
# Two calls naming the same component both define te_test_domain and fail to
# configure. The resolution is ONE call listing both files:
#
#     trading_engine_add_component_test(domain
#         domain_trade_signal_test.cpp
#         domain_fill_test.cpp)
#
# It cannot be applied on this branch, because domain_trade_signal_test.cpp
# does not exist here and CMake errors on a missing source at configure time.
# The resolution has been verified on the local branch
# `adam/adr-0004-premerged-with-jordan` (31/31 tests pass). Two further
# conflicts come with it, both resolved the same way -- keep both sides:
# docs/adr/README.md (the ADR index table) and tests/fixtures/README.md.
trading_engine_add_component_test(domain
    domain_fill_test.cpp)

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
