# Test fixtures

Static data used by unit and integration tests. **Do not** put live credentials
or large binary dumps here.

| File / dir            | Purpose                                                      |
|-----------------------|-------------------------------------------------------------|
| `signals/*.json`      | **Committed.** Proposed Strategy→Risk signal examples (draft) — see below. |
| `alpaca/*.json`       | Planned. Recorded Alpaca REST/WebSocket payloads for parser tests |
| `market_data/*.csv`   | Planned. Small deterministic bar/trade series for replay tests |
| `expected/*.json`     | Planned. Golden `PerformanceReport` / snapshot outputs       |

Keep every fixture small (a few KB) and human-readable so diffs are reviewable.
Fixture schemas are not final -- see [`docs/OPEN_QUESTIONS.md`](../../docs/OPEN_QUESTIONS.md).

## `signals/` — proposed examples for ADR 0002 (draft)

`new_order_market.json`, `new_order_limit.json`, and `cancel.json` illustrate
the Strategy Engine -> Risk Manager contract proposed in
[`docs/adr/0002-strategy-risk-signal-contract.md`](../../docs/adr/0002-strategy-risk-signal-contract.md)
(status: **Proposed**, not Accepted -- tracks GitHub issue #3).

They are **examples, not golden/expected files**: no JSON (de)serialization
code exists in this repository yet, so nothing reads or round-trips these.
They were checked for JSON syntax only -- that is not the same as validating
them against the contract, since there is no contract-checking code to
validate against.
