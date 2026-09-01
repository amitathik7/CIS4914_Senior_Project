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

## Status

The file format and parser are **not decided or implemented**. `load_config()`
throws `NotImplemented`. See
[`docs/OPEN_QUESTIONS.md`](../docs/OPEN_QUESTIONS.md).
