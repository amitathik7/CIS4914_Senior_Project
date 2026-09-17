# Test fixtures

Static data used by unit and integration tests. **Do not** put live credentials
or large binary dumps here.

| File / dir            | Purpose                                                      |
|-----------------------|-------------------------------------------------------------|
| `fills/*.json`        | **Committed.** Proposed Execution→Portfolio examples (draft) - see below. |
| `alpaca/*.json`       | Planned. Recorded Alpaca REST/WebSocket payloads for parser tests |
| `market_data/*.csv`   | Planned. Small deterministic bar/trade series for replay tests |
| `expected/*.json`     | Planned. Golden `PerformanceReport` / snapshot outputs       |

Keep every fixture small (a few KB) and human-readable so diffs are reviewable.
Fixture schemas are not final -- see [`docs/OPEN_QUESTIONS.md`](../../docs/OPEN_QUESTIONS.md).

## `fills/` - proposed examples for ADR 0004 (draft)

`fill_full.json`, `fill_partial_1.json`, `fill_partial_2_completing.json` and
`order_rejected.json` illustrate the Execution Simulator -> Portfolio Manager
contract proposed in
[`docs/adr/0004-execution-portfolio-fill-contract.md`](../../docs/adr/0004-execution-portfolio-fill-contract.md)
(status: **Proposed**, not Accepted -- tracks GitHub issue #4).

The two `fill_partial_*` files are one series against a single order
(`order_id` 3306): sequence 1 leaves quantity outstanding, sequence 2
completes it. `order_rejected.json` is deliberately **not** a fill -- it shows
an execution rejection reported as an Order lifecycle event, which per that
ADR's section 7 does *not* go to the Portfolio Manager at all.

**Money and price fields are integers scaled by 10^6** (so `150020000` is
150.02), matching `NUMERIC(18,6)` in the draft schema. That scale is a
**recommendation, not a ratified decision** -- see ADR 0004 section 12, which
records that three different scales are currently in circulation and that
OQ#11 has not been settled. Quantities are shown as whole share counts, which
is the direction the team is leaning but is **not settled either** -- and it
runs opposite to ADR 0002 section 8, which proposes allowing fractional
quantities while inviting reviewers to overrule it.

They are **examples, not golden/expected files**: no serialization code exists
in this repository yet, so nothing reads or round-trips these. They were
checked for JSON syntax only -- which is not the same as validating them
against the contract, since there is no contract-checking code to validate
against. Note also that these are the *serialized* form, which only exists
under a cross-process transport; in-process the same contract travels as a
native `domain::Fill` with no JSON involved.
