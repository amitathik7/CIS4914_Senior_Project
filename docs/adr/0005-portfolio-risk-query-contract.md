# 5. Portfolio Manager → Risk Manager query contract

- Status: **Proposed** (not Accepted - draft prepared for review)
- Date: 2026-09-17
- Updated: 2026-09-20. §4 flipped to Option B by team decision: the Portfolio
  Manager reserves cash at order submission, so it exposes open orders and
  buying power. §1, §2, §6 and §8 follow from that.
- Deciders: Adam (Portfolio Manager, provider) and Amit (Execution Simulator,
  because §4 depends on where open-order state lives). The Risk Manager is
  shared ownership (Adam, Amit, Blake, Jordan), so the consumer side has no
  single owner to sign for it - this needs a team call rather than one
  approval. `COMPONENT_OWNERSHIP.md` is stale and does not reflect this.
- Related: GitHub issue #5; **ADR 0004 (issue #4), whose §8 this document is
  the other half of** - the two share one decision on open-order ownership and
  must be read together; [`../OPEN_QUESTIONS.md`](../OPEN_QUESTIONS.md) OQ#10
  (risk limits) and OQ#2/#3.
- **Depends on an unwritten ADR.** The pending Kafka decision (expected ADR
  0003, superseding ADR 0001 item 12) determines whether this stays a direct
  call. Written to survive either outcome - see §5.

> Drafted as a review starting point. The open-order decision in §4 is settled
> by the team; everything else is still a proposal. See "Sign-off".

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

This ADR therefore mostly **documents and confirms** an existing interface.
The team's decision on open-order ownership (§4) adds one method,
`buying_power()`, and three snapshot fields. Everything else the ticket asks
for was already there.

## Decision

### 1. `IPortfolioView` is the contract, with one addition

```cpp
class IPortfolioView {
    virtual domain::PortfolioSnapshot snapshot() const = 0;
    virtual std::optional<Position> position(const common::Symbol&) const = 0;
    virtual common::Money cash() const = 0;          // settled cash
    virtual common::Money buying_power() const = 0;  // NEW: cash - reserved_cash
};
```

`buying_power()` is new with the §4 decision. The snapshot gains
`reserved_cash`, `buying_power` and `pending_orders` to match.
`IPortfolioView` itself stays **read-only**: the Risk Manager cannot mutate
the portfolio through it, and cannot reach past it into persistence.

§4's atomic check-and-hold is the one mutating call the Risk Manager gets,
and it lives on a **separate** interface so that read-only property
survives:

```cpp
class IReservationLedger {
    virtual ReservationResult hold_for_signal(
        common::SignalId, const common::Symbol&, domain::OrderSide,
        common::Quantity, std::optional<common::Price>) = 0;
    virtual void release_signal_hold(common::SignalId) = 0;
};
```

The Risk Manager holds `const IPortfolioView&` for every read, plus
`IReservationLedger&` for that single call. Both are implemented by
`PortfolioManager`.

### 2. The ticket's four fields, mapped

| Ticket asks for | Provided by | Status |
|---|---|---|
| Current positions (symbol + quantity) | `PortfolioSnapshot::positions` → `Position::symbol`, `Position::quantity` | ✅ exists |
| Cash on hand ("amount we can still use to buy") | `IPortfolioView::buying_power()`, `PortfolioSnapshot::buying_power` | ✅ **new** - see §6. Settled cash stays `cash()`. |
| Total account valuation | `PortfolioSnapshot::total_equity` | ✅ exists |
| Pending / unfulfilled orders | `PortfolioSnapshot::pending_orders` → `PendingOrder::symbol`, `side`, `remaining_quantity` | ✅ **new** - §4 |

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

> **Decided by the team on 2026-09-20: Option B.** The Portfolio Manager
> reserves cash at order submission and functions as a traditional portfolio
> manager: it owns open-order state and exposes buying power. This section is
> deliberately identical to ADR 0004 §8. The two must not drift, and they changed
> together.

**Decided: the Portfolio Manager owns open-order state.**

**Why the two tickets moved together.** Reserving cash at submission means the
Portfolio Manager must track every order until it resolves. It must therefore
be told when each order is submitted, so it can reserve, and when each order
ends without filling (rejected, cancelled or expired), so it can release.
Without those events the reservation leaks and buying power drifts down
permanently. That is exactly the question ADR 0004 §7 answers, which is why the two
sections were always one decision.

**What the Portfolio Manager owns and exposes**

- Cash, positions, cost basis, realised/unrealised P&L and total equity, as
  before.
- **Open orders:** one entry per unresolved order, carrying symbol, side and
  remaining quantity. This is exactly what issue #5 asks for.
- **Reserved cash:** the total held against open buy orders.
- **Buying power:** `cash - reserved_cash`. This is the number a new buy is
  checked against. `cash()` keeps meaning settled cash, so "can I afford
  this?" is answered by buying power, not by cash.

**Why Option B: the case the team accepted**

- The Risk Manager asks one component one question and gets one answer.
- It is the standard broker model.
- One component computing cash and pending exposure together cannot have them
  disagree with each other. A Risk-side join from two sources could, because
  its view of pending orders can lag differently from its view of cash.
- It is what issue #5 literally asks for.

**Costs accepted with it**

- **Two copies of the order table.** The Execution Simulator still owns the
  authoritative table, because it has to in order to simulate fills. The
  Portfolio Manager's copy is a projection built only from the Execution
  Simulator's order events and fills. It stays consistent only if those arrive
  complete and in order, and are applied idempotently (ADR 0004 §9, ADR 0004 §11.2).
  Divergence is the main risk this option carries, and it is worse across a
  network boundary.
- A second state machine inside the Portfolio Manager, and a new write entry
  point for order lifecycle events (ADR 0004 §13).
- `PortfolioSnapshot` now mixes settled state with reserved state. Every
  consumer must use buying power, not cash, to decide whether something is
  affordable.
- `RiskContext::open_order_count` and the "open-order accounting" TODO in
  `risk_manager.hpp` should now be sourced from the Portfolio Manager's
  open-order list, or the Risk Manager ends up holding a third copy. That is a
  change to shared Risk code, so it is flagged here rather than made.

**The option not taken (Option A), for the record.** The Portfolio Manager
would have exposed settled state only, with the Risk Manager composing pending
exposure from the order stream. It kept one owner per piece of state and a
simpler Portfolio Manager, and it matched what the scaffold originally encoded
(`ExecutionSimulator::open_order_count()`, `RiskContext::open_order_count`).
It was not chosen because the team wants the Portfolio Manager to behave as a
traditional one, with reservation and buying power in one place.

**When the hold is placed: atomically, at approval.**

Checking buying power and then reserving as two steps leaves a window. An
approved signal has to reach the Execution Simulator, become an order, and
come back before the hold exists, so every signal already queued behind it is
evaluated against buying power that does not yet reflect it. That window is
measured in queue positions rather than in milliseconds, so a strategy that
emits several signals from one market event can have all of them approved
against the same cash. In a backtest at `replay_speed = 0` this is the normal
case, not a rare race.

- The Risk Manager **checks buying power and places the hold in one call**,
  under one lock. This is possible precisely because §5 keeps the Risk
  Manager and the Portfolio Manager in one process, and it is what a broker
  does at order entry.
- The hold is keyed by `SignalId` when placed, and attached to the order when
  that order's submission event arrives. `Order::origin_signal` already
  carries the correlation.
- A rejected order still carries `origin_signal`, so the hold is released even
  when the order never reached `working`.
- **The read seam stays read-only.** The mutating call lives on a separate
  narrow interface (`IReservationLedger`), so `IPortfolioView` still cannot be
  used to change portfolio state.
- The Portfolio Manager sizes the hold, not the Risk Manager, because pricing
  knowledge (last mark, fee config) already lives there.
- **Wording note:** this refines "reserved at order submission" to "held at
  approval, attached to the order at submission". What the team decided is
  unchanged, in that the Portfolio Manager owns open orders and buying power.
  The hold simply starts one step earlier, because that is what closes the
  window.
- If the Risk Manager ever stops being co-located with the Portfolio Manager
  (§5 reversed), this is not possible. The fallback is for the Portfolio
  Manager to refuse a reservation that exceeds buying power and for the
  Execution Simulator to turn that refusal into a rejected order.

**Still open under this decision**

- **How much a market buy reserves.** A limit buy reserves
  `quantity × limit_price` plus estimated fees. A market buy has no price.
  Proposed: reserve at the Portfolio Manager's last mark price for the symbol
  plus a buffer, since it already tracks marks for unrealised P&L. The buffer
  size is undecided.
- **What a sell reserves.** Proposed: no cash. Instead the open sell's
  remaining quantity is committed against the position, so the same shares
  cannot be sold twice. Whether short selling is allowed at all remains
  OQ#11's behaviour half.
- **Orphaned holds.** An approved signal that never becomes an order at all,
  because the Execution Simulator dropped it rather than rejecting it, leaves
  a hold that nothing releases. Only a run-end sweep or a timeout would catch
  it. Not designed here.

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

**The §4 decision raises the stakes.** Buying power is the number most
sensitive to staleness, because the point of reserving is to stop two
approvals spending the same cash. Synchronous reads stop the Risk Manager
reading an old snapshot, and §4's atomic check-and-hold closes the gap
between approval and reservation. **The two decisions are now coupled:**
the atomic hold is only possible because this section keeps the Risk
Manager and the Portfolio Manager in one process. Reversing this section
reopens that window, and the fallback in §4 applies.

**The interface is identical under all three.** `IPortfolioView` is a read
seam; only its implementation moves. That is why this decision does not block
the contract, and it is the strongest argument for the abstraction already in
the code.

### 6. Cash is not buying power - because cash is reserved

The ticket glosses cash as *"amount we can still use to buy"*. Under the §4
decision those are two different numbers:

- **`cash()`**: settled cash. Moves only when fills settle.
- **`buying_power()`**: `cash - reserved_cash`. What is actually free to
  commit to a new buy. This is what the ticket means, and what a risk policy
  should check a new buy against.

A policy that checks a new buy against `cash()` would ignore every open order
and approve spending money that is already spoken for. That is the mistake
this split exists to prevent.

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
- **`snapshot()` copies a vector of positions and a vector of pending
  orders.** Cheap now, and the honest answer for this project's scale. `position(symbol)` exists for the
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
| No open orders | `pending_orders` empty, `reserved_cash` 0, `buying_power == cash`. |
| Negative buying power | Representable and reportable. It means reservations exceed cash, e.g. a market buy that filled above its reservation. A policy should reject new buys, not assume it cannot happen. |
| A pending sell | Reserves no cash (§4), so it does not reduce buying power. It does commit its remaining quantity against the position. |
| A hold placed but not yet attached to an order | Counts towards `reserved_cash` and lowers `buying_power`, but does not appear in `pending_orders`, because no order exists yet. So `reserved_cash >= sum(pending_orders.reserved_cash)`. |
| Hold refused | `ReservationResult::granted` is false and nothing is held. The Risk Manager rejects the signal with `buying_power_after` as the reason. |
| Order rejected before reaching `working` | The rejected order still carries `origin_signal`, so the hold is released anyway (§4). |
| Counting open orders for `max_open_orders` | `pending_orders.size()`. `RiskContext::open_order_count` should be filled from it rather than kept separately (§4). |

### 9. Scope exclusions

- One new read method (`buying_power()`), one new interface
  (`IReservationLedger`, two methods), one new type (`PendingOrder`) and
  three new snapshot fields, all required by §4. Nothing beyond that.
- Does not change `RiskContext` or `risk_manager.hpp`; §4 says their
  open-order count should read from here, but that is shared Risk code.
- No risk limits, policies, or thresholds - that is OQ#10.
- No `PortfolioManager` internals, no threading implementation.
- Does not decide the transport (§5), the cost-basis method (§3), or the
  numeric representation (OQ#11, see ADR 0004 §12).
- Does not implement anything: `snapshot()`, `position()`, `cash()` and
  `buying_power()` all remain `common::NotImplemented`.

## Consequences

**Positive**

- The contract the ticket asks for already exists; this confirms it rather
  than inventing a parallel one.
- Answers all three open questions: call format (`IPortfolioView`), cost basis
  (yes, `average_cost`), sync vs push (sync for v1, with the interface
  unchanged under all three options).
- Gives issues #4 and #5 **one** decision on open-order ownership instead of
  two contradictory documents.
- The Risk Manager gets cash, buying power and open orders from one
  component, in one snapshot, so they cannot disagree with each other.
- The narrow read seam means the Risk Manager cannot mutate portfolio state or
  reach into persistence - enforced by the type, not by convention.

**Negative / risks**

- The snapshot now mixes settled and reserved state. A policy that reads
  `cash` where it should read `buying_power` gets the wrong answer silently
  (§6).
- Open orders now live in two places, the Execution Simulator and the
  Portfolio Manager (§4). Whatever the Risk Manager reads here is only as
  right as the order events it was built from.
- Sync pull requires Risk and Portfolio to be co-located, which conflicts with
  cross-process-everywhere if that lands. §5 names the migration path.
- The atomic hold ties this contract to §5: if the Risk Manager ever stops
  being co-located with the Portfolio Manager, the hold cannot be atomic
  and §4's fallback is needed.
- A hold whose signal never becomes an order at all leaks until a run-end
  sweep (§4). Not designed here.
- `snapshot()` returning a copy is fine at this scale and will not stay fine
  forever. No change proposed now.

## Alternatives considered

- **Option A: Portfolio exposes settled state only** (§4) - one owner per
  piece of state and no new methods. It was the proposal in earlier drafts.
  Not chosen: the team wants reservation and buying power held in one place.
- **Folding buying power into `cash()`** - no new method. Rejected, because
  it makes settled cash unreadable and hides the difference §6 exists to
  make visible.
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

Only the open-order decision is ticked. The rest is still a draft for review.

- [ ] Adam - Portfolio Manager (provider side)
- [ ] Amit - Execution Simulator (§4 depends on where open-order state lives)
- [ ] Team call on the Risk Manager side (shared ownership - no single owner)
- [x] Open-order ownership decided: Option B (team, 2026-09-20). §4 matches
      ADR 0004 §8.
- [x] Approval-to-reservation gap closed: the buying-power check and the
      hold are one atomic call at approval (team, 2026-09-20, §4).
- [ ] `RiskContext::open_order_count` filled from `pending_orders` (§4, §8).
      Shared Risk code, so a team call.
- [ ] §5 revisited once ADR 0003 (transport) exists
- [ ] `docs/adr/README.md` index updated (done in this draft; re-confirm)
- [ ] `COMPONENT_OWNERSHIP.md` real names PR landed
- [ ] Status changed from Proposed to Accepted (or Rejected / Superseded)
