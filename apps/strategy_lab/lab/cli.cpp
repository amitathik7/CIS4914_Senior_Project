#include "lab/cli.hpp"

#include <algorithm>
#include <charconv>
#include <exception>
#include <fstream>
#include <optional>
#include <system_error>

#include "lab/catalog.hpp"
#include "lab/dataset.hpp"
#include "lab/errors.hpp"
#include "lab/paths.hpp"
#include "lab/result_json.hpp"
#include "lab/session.hpp"
#include "lab/utf8.hpp"

namespace trading_engine::lab {

namespace {

constexpr std::size_t kMaxArguments        = 10'000;
constexpr std::uintmax_t kMaxResponseBytes = 1U << 20;

[[noreturn]] void usage_error(const std::string& message) {
    throw LabError(ErrorCode::UsageError, message, {Problem{0, "", "", message}}, 1);
}

// ---- @response files ------------------------------------------------------------------

void append_response_file(const std::string& token, std::vector<std::string>& out) {
    const std::string name = token.substr(1);
    if (name.empty()) {
        usage_error("'@' must be followed by the path of a response file");
    }
    const std::filesystem::path path = path_from_utf8(name);
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::is_regular_file(path, ec) ? std::filesystem::file_size(path, ec) : 0;
    if (ec || !std::filesystem::is_regular_file(path, ec)) {
        usage_error("cannot read response file '" + utf8_excerpt(name, 80) + "'");
    }
    if (size > kMaxResponseBytes) {
        usage_error("response file '" + utf8_excerpt(name, 80) + "' is larger than " +
                    std::to_string(kMaxResponseBytes) + " bytes");
    }
    std::ifstream in(path, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!in || static_cast<std::size_t>(in.gcount()) != text.size()) {
        usage_error("cannot read response file '" + utf8_excerpt(name, 80) + "'");
    }
    std::string_view view{text};
    if (view.size() >= 3 && view.substr(0, 3) == "\xEF\xBB\xBF") {
        view.remove_prefix(3);
    }

    std::size_t pos = 0;
    while (pos < view.size()) {
        const std::size_t newline = view.find('\n', pos);
        std::string_view line =
            view.substr(pos, (newline == std::string_view::npos ? view.size() : newline) - pos);
        pos = newline == std::string_view::npos ? view.size() : newline + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (line.front() == '@') {
            usage_error("response file '" + utf8_excerpt(name, 80) + "' contains another '@' argument; nested "
                        "response files are not supported");
        }
        out.emplace_back(line);
    }
}

std::vector<std::string> expand_arguments(const std::vector<std::string>& args) {
    std::vector<std::string> out;
    for (const std::string& arg : args) {
        if (!is_valid_utf8(arg)) {
            usage_error("an argument is not valid UTF-8 (on Windows put non-ASCII values in an @file)");
        }
        if (!arg.empty() && arg.front() == '@') {
            append_response_file(arg, out);
        } else {
            out.push_back(arg);
        }
        if (out.size() > kMaxArguments) {
            usage_error("more than " + std::to_string(kMaxArguments) + " arguments");
        }
    }
    return out;
}

// ---- parsing the run command -------------------------------------------------------------

struct RunCommand {
    std::string                  dataset{};
    std::vector<StrategyRequest> strategies{};
    SynchronousDemoBus::Fault    fault{SynchronousDemoBus::Fault::None};
    std::size_t                  max_rows{kDefaultMaxRows};
    bool                         no_wall_clock{false};
};

SynchronousDemoBus::Fault parse_fault(const std::string& text) {
    for (const auto fault : {SynchronousDemoBus::Fault::None, SynchronousDemoBus::Fault::RejectSignals,
                             SynchronousDemoBus::Fault::ThrowOnSignal}) {
        if (text == fault_name(fault)) {
            return fault;
        }
    }
    usage_error("--bus-fault must be none, reject-signals or throw-on-signal (got '" + utf8_excerpt(text, 40) + "')");
}

RunCommand parse_run(const std::vector<std::string>& tokens, std::size_t first, bool& pretty) {
    RunCommand command;
    bool have_dataset = false;
    const auto value_of = [&](std::size_t& i, const std::string& flag) -> const std::string& {
        if (i + 1 >= tokens.size()) {
            usage_error(flag + " needs a value");
        }
        return tokens[++i];
    };

    for (std::size_t i = first; i < tokens.size(); ++i) {
        const std::string& token = tokens[i];
        if (token == "--dataset") {
            if (have_dataset) {
                usage_error("--dataset was given more than once");
            }
            command.dataset = value_of(i, token);
            have_dataset    = true;
        } else if (token == "--strategy") {
            if (command.strategies.size() >= kMaxStrategies) {
                usage_error("at most " + std::to_string(kMaxStrategies) + " strategies per run");
            }
            command.strategies.push_back(StrategyRequest{value_of(i, token), {}});
        } else if (token == "--param") {
            const std::string& assignment = value_of(i, token);
            if (command.strategies.empty()) {
                usage_error("--param must come after the --strategy it configures");
            }
            const std::size_t equals = assignment.find('=');
            if (equals == std::string::npos || equals == 0) {
                usage_error("--param expects name=value (got '" + utf8_excerpt(assignment, 40) + "')");
            }
            command.strategies.back().params.push_back(
                ParamAssignment{assignment.substr(0, equals), assignment.substr(equals + 1)});
        } else if (token == "--bus-fault") {
            command.fault = parse_fault(value_of(i, token));
        } else if (token == "--max-rows") {
            const std::string& text = value_of(i, token);
            std::uint64_t value     = 0;
            const auto result       = std::from_chars(text.data(), text.data() + text.size(), value);
            if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size() || value == 0 ||
                value > kHardMaxRows) {
                usage_error("--max-rows must be a whole number from 1 to " + std::to_string(kHardMaxRows) +
                            " (got '" + utf8_excerpt(text, 40) + "')");
            }
            command.max_rows = static_cast<std::size_t>(value);
        } else if (token == "--pretty") {
            pretty = true;
        } else if (token == "--no-wall-clock") {
            command.no_wall_clock = true;
        } else {
            usage_error("unknown argument '" + utf8_excerpt(token, 40) + "' (see --help)");
        }
    }
    if (!have_dataset) {
        usage_error("--dataset is required");
    }
    if (command.strategies.empty()) {
        usage_error("at least one --strategy is required");
    }
    return command;
}

// ---- human-readable stderr -----------------------------------------------------------------

std::string human_error(const LabError& error) {
    std::string text = std::string{"strategy_lab_replay: error ["} + std::string{code_name(error.code())} + "]: " +
                       error.what() + "\n";
    std::size_t shown = 0;
    for (const Problem& problem : error.problems()) {
        if (shown++ == 10) {
            text += "  ... " + std::to_string(error.total_problems() - 10) + " more (see the JSON on stdout)\n";
            break;
        }
        text += "  ";
        if (problem.line != 0) {
            text += "line " + std::to_string(problem.line) + ": ";
        }
        if (!problem.where.empty()) {
            text += problem.where + ": ";
        }
        if (!problem.column.empty()) {
            text += "[" + problem.column + "] ";
        }
        text += problem.message + "\n";
    }
    return text;
}

CliResult fail(const LabError& error, bool pretty) {
    CliResult result;
    result.exit_code = exit_status(error.code());
    result.err       = human_error(error);
    try {
        result.out = error_json(error, pretty);
    } catch (const std::exception& writer_failure) {
        // The error document itself could not be written (should be unreachable: text is
        // sanitized). Fall back to a fixed, valid one rather than emit nothing.
        result.out = std::string{"{\"schema\":\"strategy_lab.error\",\"schema_version\":\"1.0\",\"error\":{\"code\":"
                                 "\"internal_error\",\"exit_status\":4,\"message\":\"could not write the error "
                                 "document\",\"total_problems\":0,\"problems\":[]}}\n"};
        result.err += std::string{"strategy_lab_replay: could not write the error document: "} +
                      writer_failure.what() + "\n";
        result.exit_code = kExitInternal;
    }
    return result;
}

}  // namespace

std::string usage_text() {
    return "usage:\n"
           "  strategy_lab_replay describe [--pretty]\n"
           "  strategy_lab_replay run --dataset <csv> --strategy <kind> [--param name=value]...\n"
           "                          [--strategy <kind> [--param name=value]...]...\n"
           "                          [--bus-fault none|reject-signals|throw-on-signal]\n"
           "                          [--max-rows N] [--pretty] [--no-wall-clock]\n"
           "  strategy_lab_replay --help\n"
           "  @<file>   replaced by that file's lines, one argument per line\n"
           "\n"
           "stdout always holds exactly one JSON document (a result or an error); stderr holds\n"
           "human-readable text. Exit status: 0 ok, 2 usage/configuration, 3 dataset, 4 internal.\n"
           "Run `strategy_lab_replay describe` for the strategies, parameters and defaults.\n";
}

CliResult run_cli(const std::vector<std::string>& args, const CliEnvironment& env) {
    bool pretty = std::find(args.begin(), args.end(), "--pretty") != args.end();   // so errors honour it too
    try {
        const std::vector<std::string> tokens = expand_arguments(args);
        pretty = std::find(tokens.begin(), tokens.end(), "--pretty") != tokens.end();   // also from an @file
        if (tokens.empty()) {
            CliResult result;
            result.exit_code = kExitUsage;
            result.err       = usage_text();
            result.out       = error_json(LabError(ErrorCode::UsageError, "no command given (see --help)",
                                                   {Problem{0, "", "", "no command given"}}, 1), pretty);
            return result;
        }
        if (tokens.front() == "--help" || tokens.front() == "-h") {
            CliResult result;
            result.err = usage_text();   // not stdout: stdout is reserved for the JSON document
            return result;
        }

        if (tokens.front() == "describe") {
            for (std::size_t i = 1; i < tokens.size(); ++i) {
                if (tokens[i] != "--pretty") {
                    usage_error("describe takes no argument other than --pretty (got '" + utf8_excerpt(tokens[i], 40) + "')");
                }
            }
            CliResult result;
            result.out = catalog_json(pretty);
            return result;
        }
        if (tokens.front() != "run") {
            usage_error("unknown command '" + utf8_excerpt(tokens.front(), 40) + "' (expected describe or run)");
        }

        const RunCommand command = parse_run(tokens, 1, pretty);

        DatasetLimits limits;
        limits.max_rows = command.max_rows;
        const Dataset dataset = load_dataset(path_from_utf8(command.dataset), limits);

        SessionOptions options;
        options.max_rows = command.max_rows;
        options.fault    = command.fault;
        const RunDocument document = run_session(dataset, command.strategies, options);

        std::optional<std::string> generated_at;
        if (!command.no_wall_clock && env.now_utc) {
            generated_at = env.now_utc();
        }

        CliResult result;
        result.out = replay_json(document, current_provenance(std::move(generated_at)), pretty);
        for (const Warning& warning : document.warnings) {
            result.err += "strategy_lab_replay: warning [" + warning.code + "]: " + warning.message + "\n";
        }
        return result;
    } catch (const LabError& error) {
        return fail(error, pretty);
    } catch (const std::exception& error) {
        return fail(LabError(ErrorCode::InternalError, std::string{"unexpected failure: "} + to_valid_utf8(error.what())),
                    pretty);
    } catch (...) {
        return fail(LabError(ErrorCode::InternalError, "unexpected failure of an unknown kind"), pretty);
    }
}

}  // namespace trading_engine::lab
