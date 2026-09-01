# Test fixtures

Static data used by unit and integration tests. **Do not** put live credentials
or large binary dumps here.

Planned contents (nothing committed yet):

| File / dir            | Purpose                                                      |
|-----------------------|-------------------------------------------------------------|
| `alpaca/*.json`       | Recorded Alpaca REST/WebSocket payloads for parser tests    |
| `market_data/*.csv`   | Small deterministic bar/trade series for replay tests       |
| `expected/*.json`     | Golden `PerformanceReport` / snapshot outputs               |

Keep every fixture small (a few KB) and human-readable so diffs are reviewable.
Fixture schemas are not final -- see [`docs/OPEN_QUESTIONS.md`](../../docs/OPEN_QUESTIONS.md).
