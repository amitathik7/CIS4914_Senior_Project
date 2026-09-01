# Integration tests -- exercise real external services and are OFF by default.
# Enable with:  cmake -S . -B build -DTRADING_ENGINE_BUILD_INTEGRATION_TESTS=ON
#
# Prerequisites (documented in docs/OPEN_QUESTIONS.md until pinned):
#   * a reachable PostgreSQL instance with the migrations applied
#     (DATABASE_URL set), and/or
#   * Alpaca API credentials (ALPACA_API_KEY / ALPACA_API_SECRET) for a
#     paper account.
#
# No integration tests exist yet. Add them with the same helper, e.g.:
#   trading_engine_add_component_test(it_persistence it_postgres_repository.cpp)
#   trading_engine_add_component_test(it_market_data it_alpaca_historical.cpp)

message(STATUS
    "trading_engine: integration tests are enabled but none are defined yet")
