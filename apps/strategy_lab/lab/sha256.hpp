#pragma once

// SHA-256 (FIPS 180-4), for dataset fingerprints, the content-addressed run id and the
// hash of a result document. A change detector with a standard, independently
// checkable definition; it is not used for anything security-sensitive.

#include <string>
#include <string_view>

namespace trading_engine::lab {

// Lower-case hexadecimal digest (64 characters) of `data`.
[[nodiscard]] std::string sha256_hex(std::string_view data);

}  // namespace trading_engine::lab
