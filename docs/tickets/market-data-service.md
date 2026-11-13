# Market Data Service — remaining work tickets

Scope: the rest of `MarketDataService` (`include/trading_engine/market_data/market_data_service.hpp`,
`src/market_data/market_data_service.cpp`) after the MVP in commit `cc9526e`.
Source adapters (`AlpacaHistoricalSource`, `AlpacaLiveSource`), `HistoricalReplay`
and the fixture source are **not** included here; they are separate components.

Format follows `.github/ISSUE_TEMPLATE/feature_implementation.yml`. Every ticket
inherits the Definition of Done from `docs/IMPLEMENTATION_PLAN.md` (CI green,
`-DTRADING_ENGINE_WARNINGS_AS_ERRORS=ON`, GoogleTest coverage, docs updated).
Primary component for all tickets: **market_data** (owner: B, backup: A).

Suggested order: MD-1 → MD-2 → MD-3…MD-7 (any order) → MD-8 → MD-9 → MD-10 → MD-11 → MD-12.

---

## MD-1 — Reject NaN and infinite prices

- **Milestone:** M2 · **Plan item:** M2 deliverable 3 (validation)
- **Summary:** `MarketDataService` rejects NaN and ±infinity in every optional price field, not just non-positive values.
- **Motivation:** `invalid_value_check` tests `*value <= 0.00`. NaN compares false, so a NaN price is currently accepted and published.
- **Design outline:** Extend `invalid_value_check` to also reject `!std::isfinite(*value)`. Keep the check inside the existing `reject_non_positive_prices` policy branch, or move non-finite checks out of the policy since they are always required (decide in the PR and note it).
- **Acceptance criteria:**
  - [ ] NaN, +inf and -inf in `price`, `bid`, `ask`, `open`, `high`, `low` are each rejected and counted in `rejected`.
  - [ ] Valid finite prices are still accepted.
- **Required tests:** Unit: one case per price field with NaN; one case with +inf; one with -inf.
- **Dependencies:** none.
- **Open questions:** none.

## MD-2 — Per-reason rejection counters

- **Milestone:** M2 · **Plan item:** M2 deliverable 3 + acceptance ("malformed rows are rejected and counted")
- **Summary:** `Stats` reports how many events were rejected for each reason, not only a single total.
- **Motivation:** Today `rejected` is one number, so a feed with bad symbols and a feed with bad prices look the same.
- **Design outline:** Add a `RejectReason` enum (`NonPositivePrice`, `NonFinitePrice`, `EmptySymbol`, `UnknownType`, `OutOfOrder`, `PriceSpike`, `CrossedMarket`, `SymbolNotAllowed`, `Duplicate`, `PublishFailed`) and a per-reason count array in `Stats`. Keep `rejected` as the total. Each validation branch records its reason. `Stats` is a public struct, so the change must be backward compatible for existing field names.
- **Acceptance criteria:**
  - [ ] Each rejection path increments exactly one reason counter and `rejected`.
  - [ ] Sum of reason counters equals `rejected`.
- **Required tests:** Unit: one rejected event per reason; assert the matching counter and that no other counter moved.
- **Dependencies:** Blocks MD-13 (metrics wiring) and should land before MD-3…MD-7 so their reasons are included.
- **Open questions:** none.

## MD-3 — Reject and count out-of-order events

- **Milestone:** M2 · **Plan item:** M2 deliverable 3 (ordering)
- **Summary:** Track the last accepted `exchange_time` per symbol. Count events that go backwards in time, and reject them when `policy.reject_out_of_order` is set.
- **Motivation:** `Stats::reordered` exists but is never incremented. `reject_out_of_order` is declared but unused. This is also a precondition for gap-detectable replay (M3 merge tie-break).
- **Design outline:** Add `std::unordered_map<Symbol, Timestamp> last_exchange_time_`. For each event: if `exchange_time < last` (strictly), increment `reordered`; if the policy is on, reject with `OutOfOrder`. Equal times are accepted. Only update `last` when the event is accepted.
- **Acceptance criteria:**
  - [ ] With the policy off, a late event is accepted and `reordered` increments.
  - [ ] With the policy on, a late event is rejected, not published, and not sequenced.
  - [ ] Ordering is tracked per symbol; one symbol's time does not affect another's.
- **Required tests:** Unit: in-order, equal-time, late-with-policy-on, late-with-policy-off, two-symbol independence.
- **Dependencies:** MD-2 (for the reason counter).
- **Open questions:** none.

## MD-4 — Price spike filter

- **Milestone:** M2 · **Plan item:** M2 deliverable 3 (validation)
- **Summary:** Reject an event whose price moves more than `policy.max_relative_price_jump` from the last accepted price for that symbol. `0` disables the filter.
- **Motivation:** The field exists in `MarketDataPolicy` but no code reads it.
- **Design outline:** Keep `std::unordered_map<Symbol, Price> last_price_`. Compare `|p - last| / last > max_jump`. The first event for a symbol has no reference and is accepted. Update the reference only on acceptance. Define which field is the reference for `Quote` events in the PR (suggest mid = (bid+ask)/2; `Trade` uses `price`).
- **Acceptance criteria:**
  - [ ] `max_relative_price_jump == 0` disables the check.
  - [ ] A move exactly equal to the threshold is accepted; a move above it is rejected with `PriceSpike`.
  - [ ] A rejected spike does not become the new reference price.
- **Required tests:** Unit: disabled; first event; at-threshold; above-threshold; rejected spike then a normal price is accepted.
- **Dependencies:** MD-2.
- **Open questions:** Quote reference price (decide in the PR; record in the header).

## MD-5 — Crossed-market check for quotes

- **Milestone:** M2 · **Plan item:** M2 deliverable 3 (validation)
- **Summary:** Reject `Quote` events where `bid > ask` when both are present.
- **Motivation:** The header lists crossed-market handling as a TODO. A crossed quote gives strategies a false price.
- **Design outline:** In the post-validation checks, for `MarketEventType::Quote` with both `bid` and `ask` set and `bid > ask`, reject with `CrossedMarket`. Equal bid and ask are accepted. Add a policy flag only if the team wants to flag instead of reject (default: reject).
- **Acceptance criteria:**
  - [ ] `bid > ask` on a Quote is rejected and not published.
  - [ ] `bid == ask` and `bid < ask` are accepted.
  - [ ] Non-Quote events are not affected.
- **Required tests:** Unit: crossed, locked (equal), normal, and a Trade with bid > ask (should be accepted, since the check is quote-only).
- **Dependencies:** MD-2.
- **Open questions:** Reject vs flag (default: reject).

## MD-6 — Symbol allow-list

- **Milestone:** M2 · **Plan item:** M2 deliverable 3 (validation); config (deliverable 7)
- **Summary:** Add `policy.allowed_symbols`. When non-empty, only listed symbols are accepted.
- **Motivation:** The header lists a symbol allow-list as a TODO. Without it, unexpected symbols from the feed reach strategies.
- **Design outline:** `std::unordered_set<Symbol> allowed_symbols` in `MarketDataPolicy`. Empty set = allow all. Check after the empty-symbol check; reject with `SymbolNotAllowed`. Build the set in the constructor so lookup is O(1) and the policy is not re-read per event.
- **Acceptance criteria:**
  - [ ] Empty allow-list accepts any symbol.
  - [ ] Non-empty list accepts listed symbols and rejects others with `SymbolNotAllowed`.
- **Required tests:** Unit: empty list; listed symbol; unlisted symbol.
- **Dependencies:** MD-2. Config mapping is covered by a separate config ticket (not in this set).
- **Open questions:** none.

## MD-7 — Duplicate suppression window

- **Milestone:** M2 · **Plan item:** M2 deliverable 3 (validation)
- **Summary:** Drop an event that is an exact duplicate of a recently accepted event for the same symbol.
- **Motivation:** The header lists a dedupe window as a TODO. Feeds can redeliver messages; duplicates inflate counts and can double-trigger strategies.
- **Design outline:** Define a duplicate as same `symbol`, `type`, `exchange_time`, `price`, `size`. Keep the last N accepted keys per symbol (fixed-size ring; N in the policy, default 0 = off). Reject with `Duplicate`. Window size is in events, not time, so behaviour is deterministic for replay.
- **Acceptance criteria:**
  - [ ] N = 0 disables dedupe.
  - [ ] An exact repeat within the window is rejected and counted as `Duplicate`.
  - [ ] An event that differs in any key field is accepted.
  - [ ] A repeat older than the window is accepted.
- **Required tests:** Unit: disabled; repeat within window; field differs; repeat after window has evicted it.
- **Dependencies:** MD-2.
- **Open questions:** Key fields (proposed above); confirm in the PR.

## MD-8 — Persist normalised events through the repository

- **Milestone:** M2–M3 · **Plan item:** M2 P1/P2 (persistence track); data tap described in the header
- **Summary:** When `repository_` is non-null, store every accepted, normalised event via `IMarketDataRepository::store` (or batch) before or after publish, as decided in the PR.
- **Motivation:** `repository_` is held but unused (`[[maybe_unused]]`). Persisted data is needed for `RepositoryMarketDataSource` replay.
- **Design outline:** Call `repository_->store(mutable_event)` after validation and stamping. Decide the failure policy: a throwing store is caught, counted as a new reason (`PersistFailed`), and the event is still published, or it is not published. Record the decision in the PR. Remove `[[maybe_unused]]` from `repository_`. Batching is a later optimisation, not in this ticket.
- **Acceptance criteria:**
  - [ ] With a repository set, every accepted event is stored once, with its final `sequence` and `ingest_time`.
  - [ ] With `nullptr`, nothing is stored and behaviour is unchanged.
  - [ ] A store failure follows the decided policy and is counted.
- **Required tests:** Unit with an in-memory fake repository: stores accepted, not rejected; `nullptr` path; throwing store path.
- **Dependencies:** The in-memory `IMarketDataRepository` double (persistence P1). Ticket for the double is owned by D.
- **Open questions:** Store-failure policy (publish or drop).

## MD-9 — Handle a failed bus publish

- **Milestone:** M2 · **Plan item:** M2 deliverable 1 (bounded bus) and acceptance (backpressure counter)
- **Summary:** Check the return value of `bus_.publish(...)`. A `false` return is counted and recorded, not ignored.
- **Motivation:** `publish` returns `bool`, and the current code discards it. Under backpressure or after shutdown, events are silently lost, and the caller's `accepted` count is wrong.
- **Design outline:** If `publish` returns false, increment `rejected` with reason `PublishFailed`, and move the `accepted` increment so it counts only successful publishes. Sequence numbers are already assigned before publish, so a failed publish leaves a detectable per-symbol gap. Document that behaviour in the header.
- **Acceptance criteria:**
  - [ ] A `false` publish is not counted as accepted and is counted as `PublishFailed`.
  - [ ] The skipped event leaves a sequence gap (documented and tested).
- **Required tests:** Unit: fake bus returning false; check counters and the sequence gap.
- **Dependencies:** MD-2. Final semantics depend on the backpressure decision.
- **Open questions:** Backpressure policy `OQ#3`. If the policy is "block", `false` will not happen here and this ticket reduces to the counter.

## MD-10 — Thread-safety contract for MarketDataService

- **Milestone:** M2 · **Plan item:** M2 deliverable 2 (shutdown) and the ground rules on threading
- **Summary:** `on_market_event()` can be called from a source I/O thread while `stats()` is called from another thread. Choose and enforce a threading contract.
- **Motivation:** The header says the scaffold is single-threaded and the future implementation must enforce its contract. `symbol_sequence_counters_`, the new per-symbol maps and `running_counts_` are all unsynchronised today.
- **Design outline:** Options: (a) document "single caller thread" and enforce it with an assert on thread id; (b) guard all state with a `std::mutex` held for the whole `on_market_event` body and in `stats()`. Option (b) is simpler and is the suggested default. Record the choice in an ADR because it is cross-cutting (`OQ#2`).
- **Acceptance criteria:**
  - [ ] The contract is written in the header and matches the code.
  - [ ] Concurrent producers do not race (run under ThreadSanitizer, if available, in CI).
  - [ ] Per-symbol sequences are unique and contiguous across threads.
- **Required tests:** Unit: N threads × M events on the same symbol; sequences are exactly 1..N×M; `accepted + rejected == N×M`.
- **Dependencies:** ADR for the queue/threading model (`OQ#2`).
- **Open questions:** `OQ#2`.

## MD-11 — Load MarketDataPolicy from configuration

- **Milestone:** M2 · **Plan item:** M2 deliverable 7 (config)
- **Summary:** The `MarketDataPolicy` fields are read from the engine config file instead of only from C++ defaults.
- **Motivation:** M2's CLI ingest path needs the policy to be tunable without recompiling.
- **Design outline:** Add a `market_data` section to the config format. Map each field, use the defaults when the key is absent, and validate ranges in `config::validate` (e.g. `max_relative_price_jump >= 0`, allow-list symbols non-empty strings). Config names only env vars for secrets; this section has none.
- **Acceptance criteria:**
  - [ ] A config with the section produces the expected `MarketDataPolicy`.
  - [ ] A config without the section uses the defaults.
  - [ ] An invalid value (negative jump) fails `validate` with a clear message.
- **Required tests:** Config unit tests for present, absent and invalid cases.
- **Dependencies:** Config format decision `OQ#7`; MD-4 and MD-6 for the fields to exist.
- **Open questions:** `OQ#7`.

## MD-12 — Unit tests for ingestion, stamping and sequencing

- **Milestone:** M2 · **Plan item:** M2 acceptance ("stamps `ingest_time` and a per-symbol `sequence`; publishes accepted events only")
- **Summary:** Add the unit tests the plan requires that are still missing. Only the two MVP tests exist today.
- **Motivation:** The plan's test list says "each validation rule + stamping". Right now only the happy path and one invalid-price case are covered.
- **Design outline:** Test-only change. Add cases for: empty symbol; `Unknown` type; sequence increments per symbol and is independent across symbols; rejected events do not advance the sequence; `ingest_time` equals `clock.now()` after the clock is advanced; `exchange_time` is not modified; `stats()` totals. Also remove the duplicate `#include <trading_engine/events/event_bus.hpp>` in the test file.
- **Acceptance criteria:**
  - [ ] Every validation rule present in the code has at least one rejection test.
  - [ ] Sequence and stamping behaviour is covered per the list above.
- **Required tests:** This ticket is the tests.
- **Dependencies:** none (can start now; extend as MD-1…MD-9 land).
- **Open questions:** none.

## MD-13 — Wire SystemMetrics counters and an ingest latency point

- **Milestone:** M2 · **Plan item:** M2 deliverable 6 (instrumentation hooks) and acceptance ("one latency measurement")
- **Summary:** `MarketDataService` records its accepted/rejected counts and one ingest latency value into `SystemMetrics`.
- **Motivation:** The header lists "counters wired to SystemMetrics" as a TODO. The plan requires metrics to exist by the end of Week 4.
- **Design outline:** Inject a `SystemMetrics*` (optional, like the repository) into the constructor. Call `increment_events(Stage::Ingest)` on accept, `increment_errors(Stage::Ingest)` on reject, and `record_latency(Stage::Ingest, elapsed)` around `on_market_event`, timed with `clock_`. Hooks only; no optimisation.
- **Acceptance criteria:**
  - [ ] Counters match `Stats` for the same run.
  - [ ] One latency value is recorded per call.
  - [ ] `nullptr` metrics works.
- **Required tests:** Unit with a fake/recording metrics object; compare counters to `Stats`.
- **Dependencies:** `SystemMetrics` implementation (currently every method throws `NotImplemented`; owned by A). MD-2.
- **Open questions:** none.

---

## Not ticketed yet (need a decision first)

- **Stale-quote timeout** (`ingest_time - exchange_time > max_age`). Plan puts heartbeat / staleness in live-resiliency stretch (M8). Ticket when M8 is scheduled.
- **Session-window filtering** (trading hours). Not named in the plan; needs a team decision on whether it is in scope.
- **Batch store** for the repository. Performance work; M8.
- **Correction / revision handling** (`MarketEvent` has no revision marker yet). Depends on the event-shape decision `OQ#1`.
