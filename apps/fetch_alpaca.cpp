#include "fetch_alpaca.hpp"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "bar_csv.hpp"
#include "trading_engine/common/errors.hpp"
#include "trading_engine/common/timestamp_text.hpp"
#include "trading_engine/configuration/engine_config.hpp"
#include "trading_engine/market_data/alpaca_historical_source.hpp"
#include "trading_engine/market_data/http_client.hpp"

namespace trading_engine::cli {

const char* const kFetchAlpacaUsage =
    "  trading_engine fetch-alpaca --symbols SYM[,SYM...] --start TIME --end TIME [options]\n"
    "\n"
    "  Downloads Alpaca historical bars into the CSV layout --bars reads.\n"
    "  Needs ALPACA_API_KEY and ALPACA_API_SECRET in the environment.\n"
    "  --start TIME            YYYY-MM-DD (midnight UTC) or RFC 3339 UTC, inclusive\n"
    "  --end TIME              YYYY-MM-DD (that whole day included) or RFC 3339 UTC, exclusive\n"
    "  --timeframe TF          1Min, 5Min, 15Min, 1Hour, 1Day, ... (default 1Day)\n"
    "  --adjustment KIND       raw | split | dividend | all (default all)\n"
    "  --feed iex|sip          data feed (default iex, the free plan's feed)\n"
    "  --out FILE              write here instead of standard output\n"
    "  --base-url URL          market data host (default https://data.alpaca.markets)\n"
    "  Exits 3 when Alpaca returned no bars for the range.\n";

namespace {

[[noreturn]] void fetch_usage_error(const std::string& message) {
    std::cerr << "trading_engine fetch-alpaca: " << message << "\n\nusage:\n" << kFetchAlpacaUsage;
    std::exit(2);
}

// date-only text means midnight UTC; for --end the next midnight, so the day is included
std::optional<common::Timestamp> parse_time(const std::string& text, bool end_of_range) {
    if (text.size() == 10 && text[4] == '-' && text[7] == '-') {
        const auto midnight = common::parse_timestamp(text + "T00:00:00Z");
        if (midnight && end_of_range) {
            return *midnight + std::chrono::hours{24};
        }
        return midnight;
    }
    return common::parse_timestamp(text);
}

std::vector<common::Symbol> split_symbols(const std::string& text) {
    std::vector<common::Symbol> symbols;
    std::string current;
    for (const char c : text + ",") {
        if (c == ',') {
            if (!current.empty()) {
                symbols.push_back(current);
            }
            current.clear();
        } else if (c != ' ') {
            current.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    return symbols;
}

}  // namespace

int fetch_alpaca(int argc, char** argv) {
    config::AlpacaConfig alpaca;
    market_data::HistoricalQuery query;
    query.timeframe = "1Day";
    query.adjustment = "all";
    std::string start_text;
    std::string end_text;
    std::string out_path;

    for (int i = 2; i < argc; ++i) {
        const std::string flag = argv[i];
        if (i + 1 >= argc) {
            fetch_usage_error(flag + " needs a value");
        }
        const std::string value = argv[++i];
        if (flag == "--symbols") {
            alpaca.symbols = split_symbols(value);
        } else if (flag == "--start") {
            start_text = value;
        } else if (flag == "--end") {
            end_text = value;
        } else if (flag == "--timeframe") {
            query.timeframe = value;
        } else if (flag == "--adjustment") {
            query.adjustment = value;
        } else if (flag == "--feed") {
            if (value != "iex" && value != "sip") {
                fetch_usage_error("--feed needs iex or sip");
            }
            alpaca.data_feed = value;
        } else if (flag == "--out") {
            out_path = value;
        } else if (flag == "--base-url") {
            alpaca.market_data_base_url = value;
        } else {
            fetch_usage_error("unknown option " + flag);
        }
    }

    if (alpaca.symbols.empty()) {
        fetch_usage_error("needs --symbols");
    }
    const auto start = parse_time(start_text, false);
    const auto end = parse_time(end_text, true);
    if (!start) {
        fetch_usage_error("--start needs YYYY-MM-DD or an RFC 3339 UTC time");
    }
    if (!end) {
        fetch_usage_error("--end needs YYYY-MM-DD or an RFC 3339 UTC time");
    }
    query.start = *start;
    query.end = *end;

    try {
        market_data::validate_alpaca_query(alpaca, query);
    } catch (const common::ConfigError& e) {
        fetch_usage_error(e.what());
    }

    if (!market_data::http_client_available()) {
        std::cerr << "trading_engine fetch-alpaca: this build has no libcurl; install it (brew install curl / apt "
                     "install libcurl4-openssl-dev / vcpkg install curl:x64-windows) and reconfigure\n";
        return 1;
    }

    const market_data::AlpacaCredentials credentials = market_data::resolve_alpaca_credentials(alpaca);
    const auto http = market_data::make_default_http_client();

    market_data::AlpacaFetchOptions options;
    options.on_page = [](std::size_t pages, std::size_t bars) {
        std::cerr << "page " << pages << ": " << bars << " bars so far\n";
    };

    const std::vector<domain::MarketEvent> bars =
        market_data::fetch_alpaca_bars(*http, credentials, alpaca, query, options);

    if (out_path.empty()) {
        write_bar_csv(std::cout, bars);
    } else {
        std::error_code mkdir_error;  // creates a missing folder such as data/; a real failure shows at open
        std::filesystem::create_directories(std::filesystem::path{out_path}.parent_path(), mkdir_error);
        std::ofstream out{out_path};
        if (!out) {
            std::cerr << "trading_engine fetch-alpaca: cannot write " << out_path << '\n';
            return 1;
        }
        write_bar_csv(out, bars);
        if (!out.flush()) {
            std::cerr << "trading_engine fetch-alpaca: write to " << out_path << " failed\n";
            return 1;
        }
    }

    std::cerr << "fetched " << bars.size() << " bars for " << alpaca.symbols.size() << " symbol(s)";
    if (!bars.empty()) {
        std::cerr << ", " << common::format_timestamp(bars.front().exchange_time) << " to "
                  << common::format_timestamp(bars.back().exchange_time);
    }
    std::cerr << (out_path.empty() ? "" : " -> " + out_path) << '\n';
    return bars.empty() ? 3 : 0;
}

}  // namespace trading_engine::cli
