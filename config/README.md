# Configuration

- [`config.example.json`](config.example.json) — checked-in template. Every
  field is documented by example. **Contains no secrets.**
- `config.json` — your local copy. **Gitignored.** Create it with:

  ```bash
  cp config/config.example.json config/config.json
  ```

## Secrets

Credentials are never stored in config files. The config names the environment
variables that hold them:

| Env var             | Used for                              |
|---------------------|---------------------------------------|
| `ALPACA_API_KEY`    | Alpaca Market Data API key            |
| `ALPACA_API_SECRET` | Alpaca Market Data API secret         |
| `DATABASE_URL`      | Full PostgreSQL connection URL (libpq)|
| `PGPASSWORD`        | PostgreSQL password (if not in URL)   |

Set them in your shell or a `.env` file (also gitignored); do not commit them.

## Using it

```bash
./build/bin/trading_engine run --bars data/alpaca_2024.csv --config config/config.json
```

`run --config FILE` takes the risk limits and the simulation settings
(starting cash, fees, slippage, latency) from the file. With `"mode": "live"`
and no `--bars`, it trades Alpaca's live bars for `alpaca.symbols` on
`alpaca.stream_url`, logging in with the variables `alpaca.api_key_env` and
`alpaca.api_secret_env` name ([`docs/ALPACA.md`](../docs/ALPACA.md)). Command-line flags win
over the file (`--cash 5000` replaces `simulation.starting_cash`), and the file
wins over the built-in defaults. There is no automatic discovery: without
`--config`, no file is read.

## Rules

- JSON. Every key is optional; a missing one keeps the default in
  `include/trading_engine/configuration/engine_config.hpp`, so `{}` is a valid
  file.
- An unknown key is an error, so a typo (`max_daily_los`) is not silently
  ignored. Keys starting with `$` are comments.
- Money (`starting_cash`, limits, fees) is read exactly from the number's
  text, to six decimal places. No exponents (`1e5`).
- The whole file is checked before a run starts: positive cash and limits,
  `max_position_notional` no larger than `max_gross_exposure`, feed `iex` or
  `sip`, `wss://`/`ws://` stream URL, a log level the engine knows, at least
  one symbol in live mode. Errors name the file and field:

  ```
  configuration error: config/config.json: risk.max_daily_loss: must not be negative
  ```

`config::load_config` (and `parse_config` for text in memory) implement this;
the tests are in `tests/unit/config_test.cpp`.
