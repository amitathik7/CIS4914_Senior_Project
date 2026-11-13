# Alpaca market data

The engine can download real historical stock bars from Alpaca's market data
API and run them through the same pipeline as the recorded CSV files. The live
WebSocket feed is not built yet.

## One-time setup

1. Create a free account at alpaca.markets and choose the **Trading API**
   (not the Broker API, which is for companies building brokerages).
2. Switch to the **Paper** account and generate a key on the home page's
   API Keys panel. The secret is shown once.
3. Put the pair in your shell, never in a file in this repo:

   ```bash
   export ALPACA_API_KEY=your-key-id
   export ALPACA_API_SECRET=your-secret
   ```

   (PowerShell: `$env:ALPACA_API_KEY = "..."`.)

The build needs libcurl. macOS has it; on Linux install `libcurl4-openssl-dev`;
on Windows `vcpkg install curl`. CMake prints `libcurl: ... (Alpaca REST
enabled)` when it finds it. Without it everything still builds and
`fetch-alpaca` says how to fix it.

## Download bars

```bash
./build/bin/trading_engine fetch-alpaca --symbols AAPL,MSFT,SPY \
    --start 2024-01-01 --end 2024-12-31 --timeframe 1Day --out data/alpaca_2024.csv
```

| Option | Meaning |
|---|---|
| `--symbols` | Comma-separated tickers (case does not matter) |
| `--start` / `--end` | `YYYY-MM-DD` (the end day is included) or RFC 3339 UTC (`2024-03-01T14:30:00Z`, end exclusive) |
| `--timeframe` | `1Min`, `5Min`, `15Min`, `1Hour`, `1Day`, `1Week`, `1Month` (default `1Day`) |
| `--adjustment` | `raw`, `split`, `dividend`, `all` (default `all`, so splits do not look like crashes) |
| `--feed` | `iex` (default, free plan) or `sip` (paid plan) |
| `--out` | Output CSV; standard output when omitted |

Exit status: 0 with bars, 3 when Alpaca had no bars for the range, 2 for a
usage error, 1 for anything else (bad keys, network, plan limits), with
Alpaca's own message.

Then use the file anywhere a CSV of bars is accepted:

```bash
./build/bin/trading_engine run --bars data/alpaca_2024.csv
python start.py --bars data/alpaca_2024.csv
```

## How it works

`market_data::AlpacaHistoricalSource` (in `trading_engine_alpaca_adapter`)
calls `GET /v2/stocks/bars`, follows `next_page_token` until the range is
done, and returns every bar sorted by time, then symbol. Prices are parsed
from the JSON text into `common::Decimal` exactly (six decimals). Rate limits
(HTTP 429), server errors and dropped connections are retried up to six times
with a doubling backoff from one second; authentication and plan errors are
not retried.

The HTTP client is behind `market_data::IHttpClient`, so the unit tests
(`tests/unit/alpaca_historical_test.cpp`) run on recorded responses in
`tests/fixtures/alpaca/` with no network and no credentials.

## Limits

- The free plan serves the IEX feed, about 2-3% of US volume, so minute bars
  for thinly traded symbols have gaps. Daily bars are complete enough for
  backtests.
- The free plan cannot query SIP data from the last 15 minutes.
- Requests are limited to 200 per minute; one request returns up to 10,000
  bars, so even years of minute bars take a handful of requests.
