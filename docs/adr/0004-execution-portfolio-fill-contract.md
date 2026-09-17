# 4. Execution Simulator → Portfolio Manager fill contract

- Status: **Proposed** (not Accepted - draft prepared for review)
- Date: 2026-09-17
- Deciders: Adam (Portfolio Manager) and Amit (Execution Simulator). This is a
  genuine two-party contract - the two components are owned by two different
  people. It changes `domain/` value types (`Fill`, `Order`), which per
  [`../COMPONENT_OWNERSHIP.md`](../COMPONENT_OWNERSHIP.md) needs two approvals
  including the owner of the component that most depends on the type; both of
  those people are the deciders above. Note that the ownership document is
  **stale** - it still carries `Member A/B/C/D` placeholders and describes
  execution + portfolio as a single area under one owner, which is not how the
  team is actually organised. Correcting it is out of scope here but should
  happen before this ADR is marked Accepted.
- Related: GitHub issue #4 (this contract) and issue #5 (Portfolio Manager →
  Risk Manager), which share one position on open-order ownership - see §8;
  ADR 0002 (Strategy → Risk, **Proposed**) whose §0 this document partly
  contradicts - see §0.3; [`../OPEN_QUESTIONS.md`](../OPEN_QUESTIONS.md) OQ#11
  (numeric representation, **blocking** - see §12) and OQ#2/#3 (queue,
  backpressure).
- **Depends on an unwritten ADR.** The team has decided to move the pipeline
  onto Kafka, which supersedes ADR 0001 item 12 ("in-process, single-node
  engine. No distribution, no IPC."). That reversal is **not yet recorded
  anywhere** and is expected to become ADR 0003. This document is written to
  survive either outcome - see §0.

> Drafted as a review starting point. See "Sign-off" at the bottom - nothing
> there is checked yet, and nothing in this file should be read as approved.

## Context

Issue #4 asks for the shape of what the Execution Simulator hands the
Portfolio Manager: order id, symbol, side, fill status, quantity filled, fill
price, timestamp - in JSON. It asks one open question: *do rejected orders get
reported to the Portfolio Manager, or only successful/partial fills?*

The scaffold already answers some of this and contradicts itself on the rest:

- `domain::Fill` ([`fill.hpp`](../../include/trading_engine/domain/fill.hpp))
  already carries `id` (`FillId`), `order_id`, `symbol`, `filled_quantity`
  (**signed**, `+buy / -sell`), `fill_price`, `fees`, `slippage` and
  `filled_at`. It carries **no status** and **no side** - five of the ticket's
  seven fields already exist, which makes the schema half of this ticket
  largely clerical. The interesting content is elsewhere.
- `domain::Order` ([`order.hpp`](../../include/trading_engine/domain/order.hpp))
  already has an `OrderStatus` enum including `Rejected`, `Cancelled`,
  `Expired` and `PartiallyFilled`, and a `TODO` for a reject reason.
- `PortfolioManager`
  ([`portfolio_manager.hpp`](../../include/trading_engine/portfolio/portfolio_manager.hpp))
  has exactly two write entry points: `apply(const Fill&)` and
  `mark(const MarketEvent&)`. **There is no way to hand it an Order.**
- But [`../DATA_FLOW.md`](../DATA_FLOW.md) line 35 says
  `EventBus ── Order / Fill ──▶ PortfolioManager` - the data-flow document
  already routes Orders to the Portfolio Manager, i.e. already answers this
  ticket's open question "yes", while the header makes it impossible. **The
  docs and the code disagree today**, and issue #4 is sitting on that crack.
  This ADR resolves it in favour of the header (§7).
- `RiskContext` ([`risk_policy.hpp`](../../include/trading_engine/risk/risk_policy.hpp))
  carries `open_order_count` - deliberately **not** on `PortfolioSnapshot` -
  and `risk_manager.hpp`'s TODO lists "open-order accounting" as the Risk
  Manager's own future work. `ExecutionSimulator` has `open_order_count()`.
  `PortfolioSnapshot` has no open-order or buying-power field at all. So the
  existing code places open-order state in Execution and Risk, **not** in
  Portfolio. Issue #5 proposes the opposite. See §8.
- Everything on this path is still `common::NotImplemented`
  (`ExecutionSimulator::submit`, `PortfolioManager::apply`,
  `NaiveFillModel::simulate`), so no fill has ever actually flowed. This is
  pre-M5 scaffold, not a running pipeline.

## Decision

### 0. Transport neutrality - read this first

The team has committed to Kafka but has not yet ratified it in an ADR, and the
alternative of keeping `IEventBus` as the component-facing seam with Kafka as
an adapter behind it (the pattern already used for Alpaca and PostgreSQL) is
still live. This document is therefore split deliberately:

- **§1–§9 are the logical contract.** They define what information crosses the
  boundary and what it means. They are true whether the handoff is a function
  call, an in-process event bus, or a Kafka topic.
- **§10–§11 are transport bindings.** Everything that depends on *how* the
  message travels - serialization, delivery semantics, ordering, partitioning,
  schema versioning - is quarantined there.

If the transport decision changes, §10–§11 change and §1–§9 do not. That is
the point of the split, and it is why this ADR can be reviewed now rather than
waiting on ADR 0003.

#### 0.1 What is *not* being decided here

Not the transport. Not the broker. Not the serialization library. This ADR
consumes that decision; it does not make it.

#### 0.2 The logical contract is not "JSON"

Issue #4 says "Format: JSON to start." JSON is a *serialization* of this
contract, relevant only under a cross-process transport (§11). Under an
in-process transport (§10) the same logical contract is carried as a native
`domain::Fill` value with no serialization at all. Treating JSON as the
contract itself conflates the message with its encoding.

#### 0.3 This partly contradicts ADR 0002 §0 - Jordan should know

ADR 0002 (Strategy → Risk, also **Proposed**) proposes reading "JSON" in these
tickets as documentation-and-persistence only, explicitly *not* as the runtime
mechanism, and asks reviewers to confirm that reading. The Kafka decision
reverses it. That reversal belongs in ADR 0003, not here, but **ADR 0002 is
unmerged and its author can amend it himself** - which is much cheaper than
this ADR publicly overruling a sibling document. Flagged as a coordination
item, not resolved here.

### 1. The logical message: one shape, `Fill`

The Execution Simulator reports **fills only** to the Portfolio Manager. One
message shape, not three. A fill is, by definition, quantity that executed;
the ticket's "failed" is not a fill and is handled in §6–§7.

| Field | Type | Required | Meaning | Maps to (C++) |
|---|---|---|---|---|
| `fill_id` | integer | yes | Unique per run. **The idempotency key** (§9.3). | `Fill::id` (existing) |
| `order_id` | integer | yes | The order this fill executed against. | `Fill::order_id` (existing) |
| `run_id` | integer | yes | Which backtest/live session produced this fill. Redundant in-process; required once runs share a topic (§11). | `Fill::run_id` (**new**) |
| `symbol` | string | yes | e.g. `"AAPL"`. | `Fill::symbol` (existing) |
| `side` | string | yes | `"buy"` \| `"sell"`. | *derived* - see §4 |
| `fill_status` | string | yes | `"filled"` \| `"partially_filled"` - state of the **order** after this fill. | `Fill::status` (**new**) |
| `sequence` | integer | yes | 1-based index within this order's fill series (§9.2). | `Fill::sequence` (**new**) |
| `filled_quantity` | integer | yes | **> 0** - magnitude only; direction comes from `side`. Whole shares, pending §12. | `Fill::filled_quantity` (existing, signed - §4) |
| `fill_price` | scaled integer | yes | > 0. **Gross**, fees excluded (§3). Scale per §12. | `Fill::fill_price` (existing) |
| `fees` | scaled integer | yes | **>= 0**. Always reduces cash. (§2) Scale per §12. | `Fill::fees` (existing) |
| `slippage` | scaled integer | optional | Informational only. **Must not be applied to cash** (§3). Scale per §12. | `Fill::slippage` (existing) |
| `filled_at` | string, RFC 3339 UTC | yes | e.g. `"2026-09-16T14:32:06.000000000Z"`. | `Fill::filled_at` (existing) |
| `schema_version` | integer | yes | Transport-only; see §11.4. | *(no field)* |
| `message_type` | string | yes | `"fill"`. Transport-only; see §11.4. | *(no field)* |

### 2. What the ticket's field list got wrong

The proposed seven fields are not sufficient, and one of them is redundant.

- **`fees` is missing, and cash is wrong without it.** The Portfolio Manager's
  entire job on this path is cash and positions. Fees reduce cash on buys
  *and* sells. `Fill::fees` already exists; the ticket simply omits it.
- **`fill_id` is missing.** It is the idempotency key (§9.3) and already the
  primary key of the draft `fills` table in
  [`001_initial_schema.sql`](../../database/migrations/001_initial_schema.sql).
  Without it a redelivered fill is undetectable.
- **`side` duplicates information already encoded in the sign of
  `filled_quantity`** - see §4.
- **Gross vs net is unspecified**, which makes `fill_price` ambiguous - see §3.

### 3. `fill_price` is GROSS; fees and slippage are separate

**Decision: `fill_price` is the gross execution price per unit. Fees are not
folded into it.**

The cash impact of a fill is exactly:

```
cash_delta = -(signed_quantity * fill_price) - fees
```

where `signed_quantity` is `+filled_quantity` for a buy and `-filled_quantity`
for a sell. A buy reduces cash by notional *plus* fees; a sell increases cash
by notional *minus* fees. Fees always reduce cash.

Reasoning: `Fill` already has a separate `fees` field and the draft schema
already has a separate `fees` column, so a net price would double-count.
More importantly, a net price is not comparable with market prices, which
would break slippage analysis and the M5 acceptance criterion that requires
hand-computed P&L asserted to the cent.

**Under scaled integers the formula needs explicit scale arithmetic.** If
prices are integers scaled by `S` and quantity is a whole share count, then
`signed_quantity * fill_price` comes out at scale `S`, so `fees` must be at
scale `S` for the subtraction to be meaningful. Mixing scales does not fail
loudly - it produces a plausible number that is wrong by a factor of ten or a
thousand. **Every money and price field in this contract carries the same
scale**, and the scale is a property of the contract, not of each field.

The corollary is that the wire scale and the C++ numeric type are coupled: a
scaled-integer wire format feeding `double` C++ fields reintroduces exactly
the rounding the scaling was meant to remove. The two need to be decided
together - see §12.

**`slippage` is derived and must never be applied to cash.** It is documented
in the existing header as `fill_price - reference_price` - it is *already
inside* `fill_price`. Applying it again is a silent double-count. Called out
explicitly because it is an easy and expensive mistake to make.

### 4. Side: explicit on the wire, derived in C++

The ticket asks for a `side` field. `Fill::filled_quantity` is already signed
(`+buy / -sell`), so adding a C++ `side` member would create **two mutable
sources of truth for direction that can disagree**.

**Decision:**
- **In C++**, the sign of `filled_quantity` remains the single source of
  truth. No `side` member is added.
- **On the wire**, `filled_quantity` is a positive magnitude and `side` is
  explicit, because a consumer in another language should not have to infer
  direction from a minus sign.
- The mapping is one line: `signed = (side == "sell") ? -filled_quantity : +filled_quantity`.

**A known inconsistency this does not fix.** The codebase is already of two
minds: `Order::quantity` is `> 0` with direction via `side`; `Fill::filled_quantity`
is signed; `Position::quantity` is signed. Harmonising them would touch every
component and is out of scope for a two-party interface ticket - but it should
be recorded as a smaller item in `OPEN_QUESTIONS.md` rather than discovered
later.

### 5. `fill_status` describes the ORDER, not the fill

`"filled"` means *this fill completed the order's remaining quantity*;
`"partially_filled"` means *quantity remains*. The status is a property of the
order's lifecycle, observed at the moment of the fill.

This makes it **derived, denormalised data** - the authoritative running total
is `Order::filled_quantity`, held by the Execution Simulator. The two can
disagree if a fill is replayed or applied out of order.

**Tradeoff, stated rather than hidden.** Including it means each fill is
self-describing: the Portfolio Manager can tell that an order is complete
without tracking order state, which is what keeps §8's answer viable. Omitting
it would be more normalised but would force every consumer to hold order
state. Recommendation: include it, accept the denormalisation, and never treat
it as authoritative for anything other than "is this order done."

### 6. Three kinds of failure, not one

The ticket's flat `successful / partial / failed` collapses three unrelated
things:

| # | Kind | Where it happens | Does an `Order` exist? | Crosses this boundary? |
|---|---|---|---|---|
| 1 | **Risk rejection** | `RiskManager` denies the signal | No - never constructed | **No.** The Execution Simulator never sees it. |
| 2 | **Execution rejection** | `ExecutionSimulator` cannot fill: no market context, limit never marketable, no modelled liquidity | Yes, `OrderStatus::Rejected` | It is on this boundary - but as an Order event, not a Fill (§7) |
| 3 | **Terminal non-fill** | Order `Cancelled` or `Expired` | Yes | Same as #2 |

Only #2 and #3 originate from the Execution Simulator at all. #1 is a
different contract with a different counterparty, and if Analytics wants
rejected-signal data it must come from the Risk Manager - worth knowing before
someone assumes this contract delivers it.

**Consequence for the shape:** a rejection has no fill price, no filled
quantity, and no `FillId`. Modelling it as a `Fill` with a `"failed"` status
would produce a message where half the fields are meaningless - exactly the
anti-pattern ADR 0002 rejected for `side: "cancel"`. Instead, #2 and #3 are
reported using the **existing `Order` struct** with its existing
`OrderStatus`, plus the `reject_reason` its TODO already anticipated. No new
type is needed.

### 7. The open question: proposed answer is no, rejections do not go to the Portfolio Manager

> **Proposed, not decided.** This section and §8 are one decision, and the
> team settles both together. Everything below assumes Option A in §8; under
> Option B the answer flips to "yes" and the rest of this section is void.

**Proposed: the Execution → Portfolio boundary carries fills only.**
Order lifecycle events - including all rejections, cancellations and expiries
- go to persistence, analytics and the Risk Manager, not to the Portfolio
Manager.

**The case for (proposed).** A rejected order changes no cash and no
position. The Portfolio Manager's stated responsibility is cash, positions and
P&L; a rejection touches none of them. Keeping `apply()` a pure function of
settled reality is what makes M5's "hand-computed P&L asserted to the cent"
tractable. It matches the existing header, which has no way to receive an
Order. It is also the smaller contract, and the one that stays correct if the
transport changes.

**The case against (Amit may well argue this).** If the Portfolio Manager
*reserves* cash when an order is submitted, then it must be told when that
order dies, or the reservation leaks and available cash drifts down forever
with every rejected order. Under that design, the answer flips to "yes" and it
is not a close call.

**So the real question is not about rejections at all - it is whether the
Portfolio Manager reserves cash**, which is §8's question. Proposed: **it does
not.** Reservation only matters if concurrent in-flight orders can over-commit
cash between submission and fill. At this project's order rates, with a single
strategy set and a simulated venue, that window is negligible; checking cash at
fill time is sufficient and removes an entire class of state from the
component. Whatever the team decides, the constraint is fixed:
**reservation and rejection reporting are adopted together, or not at all.**
Adopting reservation without rejection reporting is the failure mode to watch
for.

**If Option A is chosen**, this also resolves the DATA_FLOW.md / header
contradiction noted in Context, in favour of the header: **line 35 should be
corrected** from `Order / Fill ──▶ PortfolioManager` to
`Fill ──▶ PortfolioManager`, with the Order edge redirected to
persistence/analytics/risk. **If Option B is chosen**, line 35 was right all
along and the header gains an order-lifecycle entry point instead.

### 8. Open-order ownership - the shared position with issue #5

Issue #5 asks the Portfolio Manager to expose *"Pending/Unfulfilled orders
(symbols + quantity + side)"* to the Risk Manager. That directly contradicts
§7 and contradicts the existing code. Both tickets need **one** answer.

> **This section is a proposal, not a decision. It is deliberately identical
> to ADR 0005 §4 - the two must not drift.** The team decides; neither author
> settles it alone. Whichever option is chosen, **both ADRs change together**:
> they are two halves of one decision, not two independent calls.

**Proposed: the Portfolio Manager does not own open-order state.**

**What decides it: does the Portfolio Manager reserve cash at order
submission?** If it does not, a pending order does not affect any number the
Portfolio Manager reports, so there is nothing to expose and nothing to
report on rejection. If it does, the Portfolio Manager must track every order
until it resolves - and must therefore be told about every rejection,
cancellation and expiry, or the reservation leaks and available cash drifts
down permanently. That is the same question §7 answers, which is why the two
sections stand or fall together.

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
  events, splits `cash()` into cash and available-to-spend, and flips §7.

**Why Option A is the one written up as proposed:** it is what the existing
scaffold already encodes, and reservation is optional at this project's order
rates - over-committing cash requires concurrent in-flight orders exceeding
available cash, which is a narrow window with one strategy set and a simulated
venue. That is a judgement about this project's scale, not a general claim
that Option A is better.

### 9. Partial fills

#### 9.1 Completion
An order is complete when a fill arrives with `fill_status: "filled"`. The
Portfolio Manager does not compute completion by summing quantities against an
order it does not track - that is what §5's status field is for.

#### 9.2 Ordering
`sequence` is 1-based per order. Its purpose is **gap detection**: a consumer
that sees `sequence` 1 then 3 knows a fill is missing and can refuse to
proceed, rather than silently computing a wrong average cost. Average cost
basis is order-dependent, so a missing or reordered fill corrupts it silently
- which is precisely the kind of bug that surfaces in week 12.

Cash, by contrast, is additive and therefore order-independent; only cost
basis genuinely requires ordering.

**Reviewer note:** `sequence` is redundant when the transport already
guarantees ordered exactly-once delivery to a single consumer. It is the one
field in this contract a reviewer might reasonably cut. It is proposed anyway
because it costs four bytes and converts a silent corruption into a loud
failure.

#### 9.3 Not double-counting a redelivered fill
**`fill_id` is the idempotency key, and deduplication lives in the Portfolio
Manager**, at the `apply()` boundary - not in the transport and not in the
producer. Rationale: the Portfolio Manager is the component whose state is
corrupted by double application, and a consumer that cannot be made to apply
something twice is correct under *any* delivery guarantee.

`PortfolioManager::apply()` is therefore proposed to return `bool`: `true` if
applied, `false` if recognised as a duplicate and ignored. A duplicate is an
**expected** condition under at-least-once delivery, not an error, so throwing
would be wrong.

The minimum viable implementation is a last-applied watermark persisted
alongside portfolio state - not an unbounded in-memory set, which is empty
after exactly the restart where it is needed. Implementation is out of scope
for this ADR; the *contract* is that `apply()` is idempotent on `fill_id`.

### 10. Transport binding A - in-process

If the pipeline stays in-process (or Kafka sits behind `IEventBus` as an
adapter, the pattern used for Alpaca and PostgreSQL):

- The message is a native `domain::Fill` carried in the existing
  `EventPayload` variant. **No serialization, no JSON, no schema version.**
- Ordering is free: one bus-worker thread delivers in publish order.
- Redelivery cannot occur unless retry is deliberately built, so §9.3's
  dedupe is cheap insurance rather than load-bearing.
- `ManualClock` continues to make backtests deterministic.

### 11. Transport binding B - Kafka

If every boundary is cross-process, the following apply **in addition to**
§1–§9. All of it is new surface that does not exist today.

#### 11.1 What changes versus in-process
| Concern | In-process | Kafka |
|---|---|---|
| Delivery | A call that happens or throws | **At-least-once.** A successful produce means "accepted by a partition", not "applied by the consumer" |
| Ordering | Total, free, one consumer thread | **Per-partition only** |
| Duplicates | Effectively impossible | **Expected.** §9.3 becomes load-bearing, not insurance |
| Typing | Closed `std::variant`, compile-time exhaustive | Runtime schema checks; the compile-time guarantee ADR 0001 valued is gone |
| Time | One `IClock`/`ManualClock` governs everything | Each process has its own clock |
| Determinism | `replay_speed = 0` reruns identically | **Lost by default** - see Consequences |
| Failure modes | Exceptions | Broker unavailable, producer buffer full, consumer lag, rebalance, offset-commit failure, poison message - none covered by `common/errors.hpp` today |
| Backpressure | OQ#3: block / drop / reject | Producer buffer config plus consumer lag; a different problem |

#### 11.2 Ordering and partitioning - Blake needs this
- Kafka orders messages **within a partition only**, so the partition key is a
  correctness decision, not a performance one.
- Average cost basis is per-symbol and order-dependent ⇒ if the fills topic is
  partitioned, **`symbol` is the correct key**. Keying by `order_id` or round
  robin breaks cost basis. Cash is additive and tolerates interleaving.
- **Recommendation for this project: one partition on the fills topic.** Total
  order for free, no key-design bugs, and the scaling path (symbol-keyed,
  N partitions) can be documented without being built. Parallelism across
  partitions is not a constraint at this project's volumes.
- **Risk and Portfolio must be in separate consumer groups.** Two components
  that both need every fill, placed in the same group, will have the messages
  *split between them* and each will silently see roughly half. This is the
  single most likely wiring mistake on this path.
- Also needs deciding, and none of it is this ADR's call: partition count,
  consumer group per component, retention (which bounds how far a restarted
  consumer can replay), and whether the Portfolio Manager runs as exactly one
  consumer instance - which it must, if it holds in-memory position state and
  the topic is single-partition.

#### 11.3 Identifier generation
`SequentialIdGenerator` starts at 1 per process and is documented as not
thread-safe. `FillId` is minted by the single Execution Simulator, so ids stay
unique - but only as long as exactly one instance produces them. Two Execution
Simulator instances would mint colliding `FillId`s, which would silently
corrupt §9.3's dedupe. Worth stating as a constraint on deployment.

#### 11.4 Schema versioning
Now that producer and consumer deploy independently, they can disagree.
Minimum viable policy, and enough for this project:
- `schema_version` integer on every message; `message_type` names the shape.
- **Additive changes only.** Never repurpose or re-type an existing field
  name; add a new one and deprecate the old.
- Consumers **ignore unknown fields** rather than failing.

Disproportionate for 15 weeks and explicitly not recommended: Confluent Schema
Registry, Avro/Protobuf with a registry, automated compatibility gates in CI.

**This collides with OQ#11.** If `Money`/`Price`/`Quantity` move from `double`
to fixed-point or integer minor units, that is a **breaking** wire change - a
version bump plus a coordinated deploy on both sides, not an additive one.

### 12. Numeric representation - a blocking dependency, not a decision here

> **Update (2026-09-17): a direction has emerged informally, in chat, and is
> not yet an ADR.** The execution owner has proposed scaled `int64_t` - prices
> as the real value times a fixed power of ten - with whole-number share
> quantities. That is the fixed-point direction this section recommends, so
> the substance looks right. Three problems with where it currently stands:
>
> 1. **Three different scales are in circulation.** 10³ was proposed in chat,
>    10⁴ is implied by the worked example that followed, and the draft schema
>    in `001_initial_schema.sql` specifies `NUMERIC(18,6)` - 10⁶. These cannot
>    all be true. **This ADR does not pick one** - §1 and §3 state the *rule*
>    (one contract-wide scale, applied to every money and price field) without
>    committing to a number, because the number is not the contract's to
>    decide.
>
>    For what it is worth as a recommendation: **10⁶**, because it is the only
>    one already committed to an artifact, it covers four-decimal sub-penny
>    quoting with headroom for fee math, and it avoids a scale conversion at
>    the persistence boundary. `int64` at 10⁶ saturates around 9.2 × 10¹²,
>    which is not a practical limit here. **The fixtures under
>    `tests/fixtures/fills/` use 10⁶ because an example has to pick something
>    - that is illustrative, not ratified**, and they get rescaled once OQ#11
>    lands.
> 2. **Whole-number quantities are the way this is leaning, not yet settled.**
>    The chat discussion points at whole shares only, and this ADR's examples
>    follow that. It runs opposite to ADR 0002 §8, which proposes *allowing*
>    fractional quantities (citing Alpaca's fractional-share support) while
>    explicitly offering that as "a recommendation for reviewers to confirm or
>    override". If whole-shares-only holds, that section needs amending by its
>    author rather than being silently contradicted here - but nothing has
>    formally overridden it yet.
>
>    Two things follow that are also **not** decided:
>    - **Whole shares is a policy; the C++ type is separate.** `common::Quantity`
>      is `double` today. Leaving it as `double` means nothing prevents a
>      fractional quantity from being constructed - the rule would be
>      documented, not enforced. Moving it to `std::int64_t` enforces it but
>      makes the rounding below explicit and mandatory at every sizing site.
>    - **Rounding is now required, and needs an owner and a direction.**
>      `TradeSignal::target_exposure` sizes by fraction of equity, so
>      `(equity × exposure) / price` essentially never lands on a whole share.
>      Sizing happens in `ExecutionSimulator::submit(TradeSignal)`, so the
>      producer rounds - but **round-toward-zero** is the safe default, since
>      rounding up can push an order past a risk limit the Risk Manager already
>      approved against.
> 3. **It is being decided in the wrong place.** OQ#11 marks this cross-cutting
>    and requiring sign-off from every member, because it touches `domain/`,
>    the schema, portfolio accounting, execution, risk and analytics. A chat
>    message about market-data structs is not that. Issues #4, #5 and #6 will
>    each encode an assumption about it within days.


`Money`, `Price` and `Quantity` are `double` aliases today, flagged provisional
in [`types.hpp`](../../include/trading_engine/common/types.hpp) and ADR 0001
item 10. OQ#11 marks the numeric-representation ADR as due **Weeks 1–2** with
all members signing off, and IMPLEMENTATION_PLAN.md lists it as *already
fixed* by M1 and a dependency of M5.

It does not appear to be decided. That is a problem for this contract, because
M5's acceptance criterion requires a round-trip P&L *"asserted to the cent"*,
and binary floating point will produce cent-level drift in exactly that
assertion.

This ADR therefore **does not** decide the numeric type, and is written to
survive either outcome: the wire fields are JSON numbers and the C++ fields are
the `common::` aliases, so a change to those aliases propagates without
renaming anything here. But the change is **not** free - it is a breaking wire
change (§11.4) and every serialization site must be revisited by hand.

Of everything flagged in this document as "industry standard", fixed-point
money is the one this project should **not** skip: it is cheap now and prevents
the bug class most likely to appear during the demo.

### 13. The consuming interface

```cpp
// portfolio/portfolio_manager.hpp
bool apply(const domain::Fill& fill);   // idempotent on fill.id
```

- **`apply()` is the only entry point for execution results.** No
  `on_order()`, no `on_rejection()` - that is §7's answer expressed in the
  type system.
- Returns `true` if applied, `false` if a recognised duplicate was ignored
  (§9.3). Changed from `void` by this ADR.
- `mark(const MarketEvent&)` is unaffected and remains how prices move
  unrealised P&L.
- `IPortfolioView` is **unchanged**. Worth noting for the demo: the read seam
  survives all three transport options intact - only its implementation would
  move - which is a point in favour of the existing abstraction.

### 14. Edge cases worth agreeing now rather than in week 12

| Case | Proposed handling |
|---|---|
| Duplicate `fill_id` | Ignore, return `false`, count it in metrics. Expected, not an error. |
| Gap in `sequence` | Refuse to apply, raise loudly. Silent acceptance corrupts cost basis. |
| Fill for an unknown `order_id` | Apply it - the Portfolio Manager does not track orders (§8), so it cannot validate this. Log for reconciliation. |
| Zero-quantity fill | Contract violation. Should never be produced; reject if seen. Distinct from the row below - a zero-quantity *order* is legitimate, a zero-quantity *fill* is not. |
| A signal that rounds to zero shares | Reachable **if** quantities end up whole (§12): a small target exposure against a high-priced instrument floors to 0. That order can never fill, so it never reaches this boundary. It should surface as `OrderStatus::Rejected` with a reason (§6 category 2), **not** as a silently dropped signal - and per §7 it does not reach the Portfolio Manager. |
| Fractional quantity on the wire | A violation under whole-shares-only, which is where this is heading but is not settled (§12). Nothing would enforce it anyway while `common::Quantity` is `double`. |
| Negative `fees` | Contract violation (rebates are not modelled). Reject. |
| Sell exceeding held quantity | Allowed - it opens a short. `Position::quantity` is explicitly signed. Whether shorting is *permitted* is OQ#11's behaviour half, not this contract's call. |
| Fill that would drive cash negative | Apply it; the Portfolio Manager records reality, it does not veto. Prevention is the Risk Manager's job. |
| `filled_at` older than the last applied fill | Apply, but flag. Under Kafka, clock skew across processes makes this possible without any fill being out of order. |
| Order fully filled, another fill arrives | Contract violation. Reject and reconcile - this means Execution and Portfolio disagree. |
| Fill arrives after run ends | Out of scope; run lifecycle is not defined yet. |
| Fill for a different `run_id` | Ignore. Under a shared topic, another run's fills are not this portfolio's business. |
| A price that is not representable at the agreed scale | Under scaled integers, slippage in basis points applied to a price rarely lands on a representable value. **The producer rounds, once, before the Fill is emitted** - so every consumer sees the same number and reconciliation is exact. Rounding direction still needs agreeing; half-away-from-zero is the conventional default for money. |
| Rounding applied twice | A consumer that re-derives a price from bps must not round again. The emitted `fill_price` is authoritative. |

### 15. Scope exclusions

- No serialization library, no (de)serialization code, no Kafka client. The
  fixtures are examples, not golden files - nothing reads them.
- No transport decision (§0.1). No ADR 0003.
- No behaviour in `ExecutionSimulator` or `PortfolioManager` - both remain
  `common::NotImplemented`, and this ADR deliberately specifies no internals of
  either.
- No runtime validation. The rules in §1 and §14 are documented, not enforced,
  matching how the scaffold documents `requested_quantity`/`target_exposure`
  exclusivity.
- Does not resolve OQ#11 (§12), OQ#2 or OQ#3.
- Does not fix the signed/unsigned quantity inconsistency (§4).
- Does not update `DATA_FLOW.md` line 35, though §7 says it should be
  corrected - left as a follow-up so this ADR's diff stays reviewable.

## Consequences

**Positive**

- Answers issue #4's open question with a reason that generalises: the
  rejection question reduces to the reservation question, and the two must be
  decided together.
- Gives issues #4 and #5 **one** position on open-order ownership instead of
  two contradictory documents.
- Surfaces a live contradiction between `DATA_FLOW.md` and
  `portfolio_manager.hpp` that would otherwise have been found during M5
  implementation.
- The logical contract survives the unresolved transport decision, so review
  can proceed without waiting on ADR 0003.
- `apply()` being idempotent on `fill_id` is correct under every delivery
  guarantee, so it need not be revisited if the transport changes.

**Negative / risks**

- **Kafka costs deterministic backtesting**, which is a stated M5 acceptance
  criterion (*"identical on rerun"*) and a DATA_FLOW.md promise. Five
  independent consumer processes with independent clocks do not interleave
  identically. This ADR cannot fix that; it flags it for ADR 0003. The
  transport-adapter option in §10 preserves it.
- `Fill::status` and `Fill::sequence` are denormalised order state on a fill
  (§5), and can contradict `Order::filled_quantity`. Accepted deliberately;
  nothing enforces agreement.
- §7 and §8 together mean the Risk Manager must join two sources for buying
  power. That cost is real and lands on a component with shared ownership.
- If the team later adopts cash reservation without revisiting §7, reserved
  cash will leak on every rejected order. The trigger condition is stated, but
  it is a decision someone must remember to revisit.
- This ADR contradicts ADR 0002 §0 on what "JSON" means (§0.3). Two Proposed
  ADRs disagreeing is a coordination cost.

## Alternatives considered

- **A `"failed"` fill status, per the ticket's literal wording** - smallest
  diff, matches the issue text. Rejected because a rejection has no price, no
  quantity and no `FillId`, producing a message whose fields are mostly
  meaningless; §6's split keeps every field meaningful in every message.
- **Portfolio Manager owns open orders and exposes buying power** (§8) - one
  query, one consistent answer for Risk. Rejected because it duplicates state
  Execution already owns and diverges across a network boundary. This is the
  strongest alternative and the one most likely to come back in review.
- **Adding `side` to `Fill` in C++** (§4) - matches the ticket literally.
  Rejected: two mutable sources of truth for direction. The wire form carries
  it instead.
- **Adding `remaining_quantity` to `Fill`** - would let the consumer *verify*
  `fill_status` rather than trust it. Not proposed, to keep the diff tight;
  a one-field change if reviewers want the belt-and-braces version.
- **Omitting `sequence`** (§9.2) - legitimate under a single-partition,
  single-consumer topology. Kept because gap detection converts silent cost-basis
  corruption into a loud failure.
- **A closed `RejectReason` enum instead of free text** - more machine-readable,
  but risk rejections and execution rejections have different vocabularies and
  only the latter reaches `Order`. Deferred as premature.
- **Net `fill_price`** (§3) - fewer fields. Rejected: double-counts against the
  existing `fees` field and breaks price comparability for slippage analysis.

## Sign-off

Unticked - this ADR is a draft prepared for review, not an accepted decision.

- [ ] Adam - Portfolio Manager (consumer side)
- [ ] Amit - Execution Simulator (producer side)
- [ ] Issue #5 drafted consistently with §8, or §8 revised
- [ ] Jordan notified that §0.3 contradicts ADR 0002 §0
- [ ] Whole-shares-only confirmed or rejected (§12); if confirmed, Jordan
      amends ADR 0002 §8, which currently proposes the opposite
- [ ] Rounding direction for signal sizing agreed (§12) - round-toward-zero
      proposed; owned by execution, not by this contract
- [ ] ADR 0003 (Kafka, superseding ADR 0001 item 12) written and accepted
- [ ] `docs/adr/README.md` index updated (done as part of this draft;
      re-confirm on acceptance)
- [ ] `docs/OPEN_QUESTIONS.md` updated (done as part of this draft;
      re-confirm on acceptance)
- [ ] `COMPONENT_OWNERSHIP.md` real names PR landed (blocks the Deciders line)
- [ ] `DATA_FLOW.md` line 35 corrected per §7
- [ ] Status line above changed from Proposed to Accepted (or
      Rejected / Superseded)
