# 5. Portfolio Manager → Risk Manager query contract

- Status: **Proposed** (not Accepted - draft prepared for review)
- Date: 2026-09-17
- Deciders: Adam (Portfolio Manager, provider) and Amit (Execution Simulator,
  because §4 depends on where open-order state lives). The Risk Manager is
  shared ownership (Adam, Amit, Blake, Jordan), so the consumer side has no
  single owner to sign for it - this needs a team call rather than one
  approval. `COMPONENT_OWNERSHIP.md` is stale and does not reflect this.
- Related: GitHub issue #5; **ADR 0004 (issue #4), whose §8 this document is
  the other half of** - the two share one position on open-order ownership and
  must be read together; [`../OPEN_QUESTIONS.md`](../OPEN_QUESTIONS.md) OQ#10
  (risk limits) and OQ#2/#3.
- **Depends on an unwritten ADR.** The pending Kafka decision (expected ADR
  0003, superseding ADR 0001 item 12) determines whether this stays a direct
  call. Written to survive either outcome - see §5.

> Drafted as a review starting point. See "Sign-off" - nothing is ticked.

## Context

Issue #5 asks for the API the Risk Manager uses to read live portfolio state,
so it can judge whether a new signal would create excessive risk. It asks for
four things and raises three questions.

**Most of this already exists.** `IPortfolioView`
([`portfolio_manager.hpp`](../../include/trading_engine/portfolio/portfolio_manager.hpp))
is already the narrow read seam the ticket describes, and `RiskManager`
already holds one:

```cpp
class IPortfolioView {
    virtual domain::PortfolioSnapshot snapshot() const = 0;
    virtual std::optional<Position> position(const common::Symbol&) const = 0;
    virtual common::Money cash() const = 0;
};
```

`RiskManager`'s constructor takes `const portfolio::IPortfolioView&`, and
`risk_manager.hpp` describes this as *"the required 'current portfolio state
must be available to the Risk Manager' link"*. ARCHITECTURE.md §2 calls it a
required feedback path. So the access pattern the ticket asks for - a function
call, not direct database access - is already what the scaffold does.

This ADR therefore mostly **documents and confirms** an existing interface,
proposes no new methods, and spends its effort on the one field that does not
fit and the one question that is genuinely open.

## Decision

### 1. `IPortfolioView` is the contract, unchanged

No new methods. No new types. The Risk Manager depends on exactly three calls,
and that narrowness is the point: it cannot mutate the portfolio, and it
cannot reach past it into persistence.

### 2. The ticket's four fields, mapped

| Ticket asks for | Provided by | Status |
|---|---|---|
| Current positions (symbol + quantity) | `PortfolioSnapshot::positions` → `Position::symbol`, `Position::quantity` | ✅ exists |
| Cash on hand ("amount we can still use to buy") | `IPortfolioView::cash()`, `PortfolioSnapshot::cash` | ✅ exists - see §6 |
| Total account valuation | `PortfolioSnapshot::total_equity` | ✅ exists |
| Pending / unfulfilled orders | - | ⚠️ **open - §4 proposes not providing them; the team decides** |

Two fields the ticket does not ask for are already on the snapshot and are
directly useful to risk policies: `gross_exposure` and `net_exposure`.
`config::RiskLimits` already has a `max_gross_exposure` limit, so this is the
value it is checked against.

### 3. Cost basis - yes, and it is already there

The ticket asks whether cost basis should accompany quantity. **Yes**, and no
change is needed: `Position::average_cost` already exists, alongside
`realized_pnl` and `unrealized_pnl`.

It is worth keeping for a reason beyond completeness: a risk policy that only
sees quantity cannot distinguish a position sitting at a large unrealised loss
from one at a gain. A daily-loss stop (OQ#10) needs that. Note the cost-basis
*method* - average vs FIFO vs LIFO - is still an open TODO on `Position`, and
this ADR does not settle it; it only says the field is part of the contract.

### 4. Open-order ownership - the shared position with issue #4

> **This section is a proposal, not a decision. It is deliberately identical
> to ADR 0004 §8 - the two must not drift.** The team decides; neither author
> settles it alone. Whichever option is chosen, **both ADRs change together**:
> they are two halves of one decision, not two independent calls.

**Proposed: the Portfolio Manager does not own open-order state.**

**What decides it: does the Portfolio Manager reserve cash at order
submission?** If it does not, a pending order does not affect any number the
Portfolio Manager reports, so there is nothing to expose and nothing to
report on rejection. If it does, the Portfolio Manager must track every order
until it resolves - and must therefore be told about every rejection,
cancellation and expiry, or the reservation leaks and available cash drifts
down permanently. That is the same question ADR 0004 §7 answers, which is why
the two sections stand or fall together.

**Option A - Portfolio exposes settled state only (proposed).** Cash,
positions, cost basis, realised/unrealised P&L, total equity. The Risk Manager
composes pending-order exposure from the order stream, which it consumes
anyway to maintain `open_order_count` for `RiskLimits::max_open_orders`.

- The existing code points this way in three places:
  `ExecutionSimulator::open_order_count()` (it creates orders and drives their
  status transitions, so it is the natural authority);
  `RiskContext::open_order_count`, a field on the Risk Manager's own context
  struct rather than on `PortfolioSnapshot`; and `risk_manager.hpp`'s TODO
  naming "open-order accounting" as Risk's work. `PortfolioSnapshot` has no
  such field.
- Exactly one component owns each piece of state, so there is nothing to
  diverge.
- `PortfolioSnapshot` keeps meaning "settled reality at an instant" rather
  than a mix of settled and speculative state.
- No second state machine inside the Portfolio Manager, and `apply()` stays a
  pure function of fills.
- **Cost:** the Risk Manager must join two sources - cash and positions from
  the Portfolio Manager, pending exposure from the order stream - rather than
  asking one component one question. That cost lands on a component with no
  single owner.

**Option B - Portfolio owns open orders and exposes buying power.**
`buying_power = cash - reserved`, with pending orders on the snapshot.

- The Risk Manager asks one component one question and gets one answer.
- It is the standard broker model, and how many production systems do it.
- **Its strongest argument:** one component computing cash and pending
  exposure together cannot have them disagree with each other. If Risk
  assembles them from two sources, its view of pending orders can lag
  differently from its view of cash.
- It is also what issue #5 literally asks for.
- **Cost:** duplicates state the Execution Simulator already holds, and two
  components tracking the same order table will diverge - worse across a
  network boundary. Requires a new Portfolio entry point for order lifecycle
  events, splits `cash()` into cash and available-to-spend, and flips
  ADR 0004 §7.

**Why Option A is the one written up as proposed:** it is what the existing
scaffold already encodes, and reservation is optional at this project's order
rates - over-committing cash requires concurrent in-flight orders exceeding
available cash, which is a narrow window with one strategy set and a simulated
venue. That is a judgement about this project's scale, not a general claim
that Option A is better.

### 5. Synchronous or push - the ticket's open question

**Recommendation: synchronous pull, for v1.** `RiskManager` already holds a
`const IPortfolioView&` and calls `snapshot()` when it evaluates.

The decisive argument is correctness, not latency. A risk gate that evaluates
against a stale portfolio can approve an order that breaches a limit - the
staleness window is exactly the window in which the thing you are guarding
against happens. Pull-at-evaluation-time has no such window.

Three options, in the order they would be reached for:

| Option | What it means | Cost |
|---|---|---|
| **Sync pull** (recommended) | Risk calls `snapshot()` when it evaluates | Requires Risk and Portfolio in the same process. Fine today; contradicts "cross-process everywhere" if that lands |
| **Request/response over the bus** | Reply topics, correlation ids, blocking wait | A known anti-pattern. High latency, a lot of machinery, and it reintroduces synchronous coupling anyway |
| **Local materialized view** | Risk consumes portfolio updates and keeps its own read model | Kafka-native and the right long-term shape, but **eventually consistent** - needs a staleness bound and a "snapshot too old → reject" policy, and M6's acceptance criteria would need restating |

**The interface is identical under all three.** `IPortfolioView` is a read
seam; only its implementation moves. That is why this decision does not block
the contract, and it is the strongest argument for the abstraction already in
the code.

### 6. Cash is buying power - because nothing is reserved

The ticket glosses cash as *"amount we can still use to buy"*. Those are the
same number **only under §4's Option A**, where nothing is reserved at order
submission. Under Option B they diverge and this contract needs a separate
accessor for available-to-spend. Recorded so the distinction is not lost
whichever way §4 lands.

### 7. Snapshot semantics

- **A snapshot is a copy, taken at an instant.** `PortfolioSnapshot::as_of`
  stamps when. It does not update itself; a policy holding one across a long
  evaluation is reading history.
- **One snapshot per evaluation, not one per policy.** `IRiskPolicy::evaluate`
  already takes `const PortfolioSnapshot&`, so the RiskManager takes one
  snapshot and passes the same value to every policy. Otherwise two policies
  in the same decision could see different portfolios.
- **Reads must be safe against concurrent writes.** `portfolio_manager.hpp`
  already states writes arrive on the execution/bus thread and reads come from
  the risk thread, and that the implementation *must* make this safe. The
  scaffold does neither yet. This contract depends on that guarantee.
- **`snapshot()` copies a vector of positions.** Cheap now, and the honest
  answer for this project's scale. `position(symbol)` exists for the
  single-symbol case and should be preferred where a policy only needs one.

### 8. Edge cases worth agreeing now

| Case | Proposed handling |
|---|---|
| No position in a symbol | `position()` returns `std::nullopt` - distinct from a flat position with `quantity == 0`, which means "held and closed". |
| Empty portfolio at run start | Valid: cash = starting cash, no positions, equity = cash. Not an error. |
| `snapshot()` before any fill | Same as above. Must not throw once implemented. |
| Negative cash | Representable and reportable. The Portfolio Manager records reality; preventing it is Risk's job (ADR 0004 §14). |
| Position with no mark price yet | `unrealized_pnl` is meaningless until `mark()` runs. A policy relying on equity must tolerate an unmarked position. **Open:** whether equity excludes unmarked positions or values them at cost. |
| Equity vs cash + positions | `total_equity == cash + Σ(position value)` is an M5 acceptance criterion; policies may assume it holds. |
| Two policies, one decision | Same snapshot for both - §7. |

### 9. Scope exclusions

- No new methods on `IPortfolioView`, no new domain types.
- No risk limits, policies, or thresholds - that is OQ#10.
- No `PortfolioManager` internals, no threading implementation.
- Does not decide the transport (§5), the cost-basis method (§3), or the
  numeric representation (OQ#11, see ADR 0004 §12).
- Does not implement anything: `snapshot()`, `position()` and `cash()` all
  remain `common::NotImplemented`.

## Consequences

**Positive**

- The contract the ticket asks for already exists; this confirms it rather
  than inventing a parallel one.
- Answers all three open questions: call format (`IPortfolioView`), cost basis
  (yes, `average_cost`), sync vs push (sync for v1, with the interface
  unchanged under all three options).
- Gives issues #4 and #5 **one** position on open-order ownership instead of
  two contradictory documents.
- The narrow read seam means the Risk Manager cannot mutate portfolio state or
  reach into persistence - enforced by the type, not by convention.

**Negative / risks**

- Under §4's proposed Option A, the Risk Manager must join two sources for
  buying power. Under Option B that cost disappears and others replace it.
- Sync pull requires Risk and Portfolio to be co-located, which conflicts with
  cross-process-everywhere if that lands. §5 names the migration path.
- This contract is only sound while `apply()` is fill-only (ADR 0004 §7). If
  reservation is adopted, §4 and §6 both change.
- `snapshot()` returning a copy is fine at this scale and will not stay fine
  forever. No change proposed now.

## Alternatives considered

- **Portfolio owns open orders, exposes `buying_power`** - one query, one
  consistent answer. Written up as Option B in §4 rather than rejected - it is
  a live option the team has not yet chosen between.
- **Push-based updates to a Risk-side read model** - the right Kafka-native
  shape, but eventually consistent, and staleness in a risk gate is a
  correctness bug. Revisit if ADR 0003 makes co-location impossible.
- **A wider interface** (`positions_for_strategy()`, `exposure_by_symbol()`,
  etc.) - convenient, but every method is a commitment, and policies can
  compute these from a snapshot. Kept narrow deliberately.
- **Returning a reference or pointer to live state instead of a copy** -
  avoids the vector copy, but hands the Risk thread a reference to something
  the execution thread mutates. Rejected on thread-safety grounds.

## Sign-off

Unticked - draft prepared for review, not an accepted decision.

- [ ] Adam - Portfolio Manager (provider side)
- [ ] Amit - Execution Simulator (§4 depends on where open-order state lives)
- [ ] Team call on the Risk Manager side (shared ownership - no single owner)
- [ ] §4 confirmed against ADR 0004 §8, or **both** revised together
- [ ] §5 revisited once ADR 0003 (transport) exists
- [ ] `docs/adr/README.md` index updated (done in this draft; re-confirm)
- [ ] `COMPONENT_OWNERSHIP.md` real names PR landed
- [ ] Status changed from Proposed to Accepted (or Rejected / Superseded)
