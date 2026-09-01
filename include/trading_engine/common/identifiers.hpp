#pragma once

// -----------------------------------------------------------------------------
//  Strongly-typed identifiers and a simple id generator.
//
//  Responsibility: prevent accidentally passing an OrderId where a FillId is
//  expected, and hand out unique ids for engine-created entities.
//
//  Thread-safety: SequentialIdGenerator is NOT thread-safe yet (see TODO).
// -----------------------------------------------------------------------------

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace trading_engine::common {

// A transparent wrapper around a 64-bit id. Value 0 means "unset".
template <class Tag>
struct StrongId {
    using tag_type = Tag;

    std::uint64_t value{0};

    constexpr StrongId() noexcept = default;
    constexpr explicit StrongId(std::uint64_t v) noexcept : value{v} {}

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }

    friend constexpr bool operator==(StrongId, StrongId) noexcept = default;
    friend constexpr auto operator<=>(StrongId, StrongId) noexcept = default;
};

// Distinct tag structs -> distinct, non-interchangeable id types.
struct OrderIdTag {};
struct SignalIdTag {};
struct FillIdTag {};
struct RunIdTag {};       // one backtest / live session
struct SubscriptionIdTag {};

using OrderId        = StrongId<OrderIdTag>;
using SignalId       = StrongId<SignalIdTag>;
using FillId         = StrongId<FillIdTag>;
using RunId          = StrongId<RunIdTag>;
using SubscriptionId = StrongId<SubscriptionIdTag>;

// Monotonic id source. First id handed out is 1, so a default-constructed id
// (value 0) is always distinguishable from a generated one.
//
// TODO: back `counter_` with std::atomic<std::uint64_t> before any component
//       generates ids from more than one thread; consider per-run id prefixes
//       so ids are unique across restarts when persisted.
template <class Id>
class SequentialIdGenerator {
public:
    [[nodiscard]] Id next() noexcept { return Id{++counter_}; }
    [[nodiscard]] std::uint64_t issued() const noexcept { return counter_; }

private:
    std::uint64_t counter_{0};
};

}  // namespace trading_engine::common

// Allow ids to be used as keys in unordered containers.
template <class Tag>
struct std::hash<trading_engine::common::StrongId<Tag>> {
    std::size_t operator()(
        const trading_engine::common::StrongId<Tag>& id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value);
    }
};
