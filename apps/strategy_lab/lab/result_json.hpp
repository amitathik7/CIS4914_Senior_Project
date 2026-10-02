#pragma once

// -----------------------------------------------------------------------------
//  The tool's JSON documents, schema version "1.0".
//
//  Versioning. `schema` names the document type and `schema_version` is "MAJOR.MINOR".
//  An additive change (a new optional member) bumps the minor; renaming, removing or
//  re-typing a member bumps the major. Consumers ignore unknown members and require the
//  same major. This version number belongs to THIS tool's output alone: it is not tied to
//  the approval status of docs/adr/0002-strategy-risk-signal-contract.md or of any other
//  ADR, and no ADR is an input to it. Signal members use the C++ names of the fields they
//  read (`requested_quantity`, `order_type`, `side`, `created_at`, `strategy_id`).
//
//  Three document types, always one per stdout:
//    strategy_lab.replay   { schema, schema_version, provenance, result }
//    strategy_lab.catalog  { schema, schema_version, catalog }
//    strategy_lab.error    { schema, schema_version, error }
//
//  Deterministic versus environmental. EVERYTHING under `result` is a pure function of
//  the dataset bytes and the request: the same inputs give the same bytes, on any day.
//  `provenance` is the only place a wall clock, a compiler or a tool build may appear:
//  `generated_at` (omitted with --no-wall-clock), `tool`, `project_version`, `compiler`,
//  `build`, and `result_sha256`, the SHA-256 of the COMPACT serialization of `result`
//  (in compact output, exactly the bytes after `"result":` up to the closing brace of the
//  document), so two runs can be compared by that one value whatever their provenance.
//
//  Absent optional values are OMITTED, never null. A number that cannot be written in JSON
//  (NaN, infinity) is never written: where a field can hold one, the field is omitted and
//  `<field>_status` says "nan", "inf" or "-inf"; a diagnostic value that is unavailable is
//  listed under `unavailable` with the reason.
//
//  Run identity. `result.run.run_id` is the first 16 hex digits of the SHA-256 of the
//  identity-defining inputs (schema version, dataset SHA-256, bus fault, and each
//  strategy's kind and resolved parameters). It is content-addressed, not a timestamp or
//  counter, so it is reproducible. Signal ids restart at 1 in every run (a fresh engine
//  per run), so a signal is globally identified by `signal_ref` = "<run_id>:<signal_id>".
// -----------------------------------------------------------------------------

#include <optional>
#include <string>
#include <string_view>

#include "lab/errors.hpp"
#include "lab/session.hpp"

namespace trading_engine::lab {

inline constexpr std::string_view kSchemaVersion = "1.0";
inline constexpr std::string_view kReplaySchema  = "strategy_lab.replay";
inline constexpr std::string_view kCatalogSchema = "strategy_lab.catalog";
inline constexpr std::string_view kErrorSchema   = "strategy_lab.error";

struct Provenance {
    std::string                tool{};
    std::string                project_version{};
    std::string                compiler{};
    std::string                build{};
    std::optional<std::string> generated_at{};   // RFC 3339 UTC; absent when not requested
};

// The tool's own identity plus an optional wall-clock time supplied by the caller.
[[nodiscard]] Provenance current_provenance(std::optional<std::string> generated_at);

// First 16 hex digits of the SHA-256 described above.
[[nodiscard]] std::string make_run_id(const RunDocument& document);

// Just the deterministic `result` object, compact.
[[nodiscard]] std::string result_json(const RunDocument& document);

// The full strategy_lab.replay document, ending in "\n".
[[nodiscard]] std::string replay_json(const RunDocument& document, const Provenance& provenance, bool pretty);

[[nodiscard]] std::string catalog_json(bool pretty);
[[nodiscard]] std::string error_json(const LabError& error, bool pretty);

}  // namespace trading_engine::lab
