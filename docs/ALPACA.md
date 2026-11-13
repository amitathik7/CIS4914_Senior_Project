# Alpaca market data

The engine can download real historical stock bars from Alpaca's market data
API and run them through the same pipeline as the recorded CSV files, or trade
Alpaca's live minute bars as they arrive. Every order is simulated: nothing is
sent to Alpaca's trading API.

## One-time setup

1. Create a free account at alpaca.markets and choose the **Trading API**
   (not the Broker API, which is for companies building brokerages).
2. Switch to the **Paper** account and generate a key on the home page's
   API Keys panel. The secret is shown once.
3. Run `python3 start.py`. The first time, it asks for the key ID and secret
   and saves them outside the project: the macOS Keychain, or a file only you
   can read in your user folder (`~/.config/trading_engine/alpaca.json`,
   `%APPDATA%\trading_engine\alpaca.json` on Windows). It loads them on every
   start after that; `python3 start.py --set-keys` replaces them. Never put
   them in a file in this repo: it is public.

   For `trading_engine run --alpaca-live` or `fetch-alpaca` outside
   `start.py`, set `ALPACA_API_KEY` and `ALPACA_API_SECRET` in the shell.

   Each person needs their own account: Alpaca allows one live stream per
   account, so two people on one key pair disconnect each other.

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
python3 start.py --bars data/alpaca_2024.csv
```

## Trade the live feed

```bash
./build/bin/trading_engine run --alpaca-live AAPL,MSFT,SPY --record data/live_today.csv
```

The engine connects to Alpaca's stream, logs in with the same two variables,
subscribes to minute bars and runs the usual pipeline (strategies, risk,
simulated execution, portfolio) on each bar as it arrives, printing bars,
signals and fills as they happen and the usual summary at the end.

| Option | Meaning |
|---|---|
| `--alpaca-live` | Comma-separated tickers |
| `--feed` | `iex` (default), `sip` (paid plan), or `test`: Alpaca's always-on test stream, whose only symbol is `FAKEPACA`, for trying it outside market hours |
| `--minutes N` | Stop after N minutes; otherwise Ctrl+C stops it |
| `--record FILE` | Also write every live bar to FILE, which `--bars` can replay later |

The strategy, risk and cash options of `run` (`--sma`, `--lookback`,
`--quantity`, `--cash`, `--risk`, `--bus`) apply as for a file. A config file
with `"mode": "live"` does the same without `--alpaca-live`: `run --config
config/config.json` trades its `alpaca.symbols` on its `alpaca.stream_url`
(`--feed` and `--alpaca-live` still override it).

Bars come once a minute per symbol, and only while the market is open (9:30 to
16:00 New York time, weekdays). Outside those hours use `--feed test`:

```bash
./build/bin/trading_engine run --alpaca-live FAKEPACA --feed test --minutes 5
```

A dropped connection reconnects by itself with a doubling backoff (1 s up to
30 s). Errors reconnecting cannot fix stop the run with Alpaca's message and
exit status 1: bad keys (402), a second connection when the plan allows one
(406), or a feed the plan does not include (409).

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

`market_data::AlpacaLiveSource` runs one I/O thread: connect, wait for
`connected`, send `{"action":"auth",...}`, wait for `authenticated`, send
`{"action":"subscribe","bars":[...]}`, wait for the acknowledgement, then push
every bar to its sink. The `run` command hands bars from that thread to the
run's thread through a queue, and `app::ConfiguredRun::feed()` processes each
one exactly as a replay would (the run is the same composition; the clock
follows the bars' times).

The WebSocket client (`market_data::IWebSocket`, `src/market_data/curl_websocket.cpp`)
uses libcurl only to open the TCP/TLS connection (`CURLOPT_CONNECT_ONLY`),
because the libcurl in macOS and Ubuntu is built without WebSocket support.
The RFC 6455 handshake and framing are ours (`websocket_protocol.cpp`), tested
against the RFC's own examples in `tests/unit/websocket_protocol_test.cpp`;
`tests/unit/alpaca_live_test.cpp` drives the live source through a scripted
fake socket (reconnects, timeouts, fatal errors, stop during a backoff).

## Limits

- The free plan serves the IEX feed, about 2-3% of US volume, so minute bars
  for thinly traded symbols have gaps. Daily bars are complete enough for
  backtests.
- The free plan cannot query SIP data from the last 15 minutes.
- The free plan allows one stream connection at a time: a second
  `run --alpaca-live` fails with error 406 until the first stops.
- Requests are limited to 200 per minute; one request returns up to 10,000
  bars, so even years of minute bars take a handful of requests.
