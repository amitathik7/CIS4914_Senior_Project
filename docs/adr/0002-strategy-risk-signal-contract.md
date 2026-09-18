# 2. Strategy Engine → Risk Manager signal contract (draft JSON shape)

- Status: **Proposed** (not Accepted — draft prepared for review)
- Date: 2026-09-16 (revised 2026-09-17 — see "Related" and §0/§8 below)
- Deciders: per [`../COMPONENT_OWNERSHIP.md`](../COMPONENT_OWNERSHIP.md), `domain/`
  value-type changes need two approvals including the top dependent
  component's owner. `TradeSignal` is depended on by strategy, risk, and
  execution; reading the ownership table, that's the strategy/risk area
  owner (Member C) and the execution area owner (Member D). Real names are
  not yet filled into that document. This is not one of the ownership doc's
  "all four members" cross-cutting categories (threading, numeric
  representation, platforms, logging, config, export schema, adapter
  dependencies), so full-team sign-off is not required by that rule — rest
  of the team FYI.
- Related: GitHub issue #3 ("[Architecture] Strategy Engine -> Risk Manager
  Interface Contract"); [`../OPEN_QUESTIONS.md`](../OPEN_QUESTIONS.md) OQ#9
  and OQ#11 (numeric representation — cross-cutting, all four members sign
  off); [PR #7](https://github.com/amitathik7/CIS4914_Senior_Project/pull/7)
  (ADR 0004, "Execution Simulator → Portfolio Manager fill contract,"
  **Proposed**, on an unmerged branch — not linked locally since the file
  doesn't exist on this branch), whose §0.3 and §12 raise the two points
  this revision responds to: the transport reading in §0, and the
  fractional-vs-whole-share quantity policy in §8.

> Drafted as a review starting point. See "Sign-off" at the bottom — nothing
> there is checked yet, and nothing in this file should be read as approved.
> **Revised 2026-09-17** in response to PR #7 / ADR 0004 (§0.3, §12): §0 now
> separates the logical contract from transport/encoding, and §8 aligns the
> quantity policy with ADR 0004's proposed direction instead of leaving two
> Proposed drafts disagreeing. Still nothing here is accepted.

## Context

Issue #3 asks for the shape of what a Strategy Engine hands to a Risk
Manager: symbol, side (buy/sell/cancel), quantity, order type (limit/market),
price (for limit), and a signal/order id — in JSON. It also asks whether the
same id should reach the Execution Simulator or whether the Risk Manager
should mint its own.

The scaffold already answers part of this and leaves real gaps for the rest:

- `domain::TradeSignal` ([`trade_signal.hpp`](../../include/trading_engine/domain/trade_signal.hpp))
  already carries `id` (`SignalId`), `strategy_id`, `symbol`, `side`
  (`SignalSide`: Buy/Sell/**Flat** — no Cancel), `created_at`, and exactly one
  of `requested_quantity` / `target_exposure`. It carries **no order type and
  no price** — those exist only on `domain::Order` today.
- `domain::Order` ([`order.hpp`](../../include/trading_engine/domain/order.hpp))
  already carries `type` (`OrderType`: Market/Limit), `limit_price`, and —
  importantly — `origin_signal : SignalId`, an explicit back-reference to the
  signal it was built from.
- IDs are already strongly typed and separately owned: `StrategyEngine` mints
  `SignalId`s (`signal_ids_` in
  [`strategy_engine.hpp`](../../include/trading_engine/strategy/strategy_engine.hpp));
  `ExecutionSimulator` mints `OrderId`s (`order_ids_` / `fill_ids_` in
  [`execution_simulator.hpp`](../../include/trading_engine/execution/execution_simulator.hpp));
  `RiskManager` mints neither (no id-generator member in
  [`risk_manager.hpp`](../../include/trading_engine/risk/risk_manager.hpp)) —
  it only evaluates.
- ADR 0001 already decided (item 12) that this engine is **in-process,
  single-node, no IPC** — a statement about process topology (one process,
  not several talking over sockets or pipes), not about internal data
  representation; it does not, by itself, rule out JSON as an in-process
  format. The event bus's interface
  ([`event_bus.hpp`](../../include/trading_engine/events/event_bus.hpp)) is
  designed to carry native C++ values through a closed
  `std::variant<MarketEvent, TradeSignal, Order, Fill>`, not bytes — that
  interface exists in the codebase today, though its concrete
  `InProcessEventBus` is itself still a `common::NotImplemented` stub, so no
  event, native or otherwise, is actually delivered by any code path yet.
  JSON has no equivalent at any level — no interface, no stub, no type —
  anywhere in this codebase. Separately, no JSON library is wired into the
  build (`cmake/Dependencies.cmake` only fetches GoogleTest), and
  `trading_engine_core` is architecturally barred from linking one
  (`docs/ARCHITECTURE.md` §3, §6) — a rule about which *target* such a
  library may live in, not a ban on JSON existing anywhere in the project.
- **This process-topology picture is now itself in flux.**
  [PR #7](https://github.com/amitathik7/CIS4914_Senior_Project/pull/7)
  (ADR 0004, Proposed, on an unmerged branch) reports that the team has
  indicated an intent to move the pipeline onto Kafka, which would supersede
  ADR 0001 item 12 and make every component boundary — including this one —
  cross-process. That intent **is not recorded in any accepted ADR**: the
  ADR index on that branch reserves the number (ADR 0003, "Transport: Kafka,
  superseding ADR 0001 item 12") but marks it "Not yet written," and this
  document does not treat it as settled either. See §0.
- Everything downstream of signal creation is still `common::NotImplemented`
  (`RiskManager::check`, `ExecutionSimulator::submit`,
  `StrategyEngine::on_market_event`) — this is pre-M4 scaffold, not a running
  pipeline.

This ADR proposes a JSON-shaped contract for the fields issue #3 asks about,
maps each field to an existing or new C++ member, and separates what's
already true today from what's newly proposed.

## Decision

### 0. What "JSON" means here — needs reviewer confirmation, and now has a live alternative

**This remains a proposed interpretation, not a settled fact.** The previous
version of this section proposed reading "JSON" in issue #3 as documentation
and persistence only, with the Strategy→Risk handoff carried as a native C++
call / event-bus payload — not as a wire format — and asked reviewers to
confirm that. [PR #7](https://github.com/amitathik7/CIS4914_Senior_Project/pull/7)
(ADR 0004 §0.3) reports that the team has since indicated an intent to move
the pipeline onto Kafka, which would make this boundary cross-process, and
flags that this contradicts the reading above. The flag is fair; this
revision addresses it directly rather than leaving two Proposed ADRs
disagreeing.

**Nothing about that intent is decided yet.** No ADR records it — the ADR
index on the branch that reserves the number marks it "**ADR 0003**,
Transport: Kafka, superseding ADR 0001 item 12 — Not yet written" — so ADR
0001 item 12 (in-process, no IPC) is still the only thing on record, and
that item is itself only **Proposed/Provisional**, never Accepted. Process
topology is honestly *unsettled between two proposals* right now, not
settled in either direction, and this ADR does not get to resolve that by
itself.

**What this section commits to, instead of picking a transport:**

- **The logical contract is not "JSON."** §1's fields, what each one means,
  and the invariants in §2–§8 describe what a `new_order` or `cancel`
  message *is* — true whether Strategy hands Risk a native `TradeSignal` /
  `SignalCancelRequest` in-process, or a serialized message crosses a Kafka
  topic. None of that changes with the transport.
- **Transport and encoding are a separate, still-open question.**
  In-process, the handoff stays a native C++ value through a function call
  or the existing `event_bus.hpp` closed `std::variant` — no serialization,
  no JSON, at all; that is what ADR 0001's current (Provisional) item 12
  describes and what the scaffold's types are shaped for today.
  Cross-process, if ADR 0003 is written and accepted, the message needs a
  wire encoding — and **JSON would be a candidate for that encoding, not a
  requirement of it**: Kafka carries Avro, Protobuf, or JSON equally well
  (ADR 0004 §11.4 leaves the same choice open for the fill contract, for the
  same reason). If the team's eventual choice is JSON, §1's shapes are
  positioned to become that encoding directly; if not, §1 still documents
  the logical shape whatever encoding is chosen must carry.

**Until ADR 0003 exists, this ADR's JSON is exactly what it was:
documentation and worked examples of the logical contract** (this file, and
`tests/fixtures/signals/*.json`), and a candidate format for
**persisted/logged** signals later, in the spirit of the M7 analytics
export. It is still **not asserted here** to be the runtime wire format —
that question now waits on ADR 0003 rather than on a claim this ADR could
settle alone. Native C++ domain types stay useful either way: the adapter
pattern already used for Alpaca and PostgreSQL (a vendor/transport-specific
target behind a stable interface, kept out of `trading_engine_core` per
`docs/ARCHITECTURE.md` §3, §6) applies just as well to a future Kafka
adapter converting between wire bytes and `domain::TradeSignal` —
serialization code living in an adapter, not in the core, the same way
Alpaca's and Postgres's client libraries already do.

Serialization and Kafka implementation stay out of scope for this change —
see §9. **Reviewers: please confirm or correct this reading, and in
particular confirm whether ADR 0003 needs drafting before or alongside this
ADR's acceptance.**

### 1. Message envelope

Two message shapes, distinguished by `message_type`. (A single flat shape is
a real alternative — see "Alternatives considered.")

#### `new_order`

| Field | JSON type | Required | Allowed values | Maps to (C++) |
|---|---|---|---|---|
| `message_type` | string | yes | `"new_order"` | *(no field — see §4)* |
| `signal_id` | number | yes | positive integer | `TradeSignal::id` (existing) |
| `strategy_id` | string | yes | non-empty | `TradeSignal::strategy_id` (existing) |
| `symbol` | string | yes | non-empty, e.g. `"AAPL"` | `TradeSignal::symbol` (existing) |
| `side` | string | yes | `"buy"` \| `"sell"` | `TradeSignal::side` (existing `SignalSide`, restricted to two of its three values here — §6) |
| `order_type` | string | yes | `"market"` \| `"limit"` | `TradeSignal::order_type` (**new**, reuses existing `domain::OrderType`) |
| `quantity` | number | yes | finite, `> 0`; whole numbers proposed for v1, pending approval (§8) | `TradeSignal::requested_quantity` (existing — **not** a new field) |
| `price` | number | required **iff** `order_type == "limit"`; omit entirely for `"market"` | finite, `> 0` | `TradeSignal::limit_price` (**new** — named after `Order::limit_price`, not the JSON key) |
| `created_at` | string, RFC 3339 UTC | yes | e.g. `"2026-09-16T14:32:01.123456789Z"` | `TradeSignal::created_at` (existing) |

#### `cancel`

| Field | JSON type | Required | Allowed values | Maps to (C++) |
|---|---|---|---|---|
| `message_type` | string | yes | `"cancel"` | *(no field — see §4)* |
| `signal_id` | number | yes | positive integer; this request's **own** id | `SignalCancelRequest::id` (**new type**, proposed) |
| `target_signal_id` | number | yes | positive integer; the earlier `new_order.signal_id` being cancelled | `SignalCancelRequest::target_signal_id` (**new**) |
| `strategy_id` | string | yes | should match the target's `strategy_id` | `SignalCancelRequest::strategy_id` |
| `symbol` | string | recommended | informational only — `target_signal_id` is authoritative | `SignalCancelRequest::symbol` |
| `created_at` | string, RFC 3339 UTC | yes | | `SignalCancelRequest::created_at` |

`quantity`, `order_type`, and `price` do not appear on a `cancel` message —
they aren't nulled out, they simply aren't part of this shape. That's the
main difference from the issue's literal `side: "Cancel"` wording; see §7.

### 2. Field-by-field name differences

Two JSON names deliberately don't match their C++ member name:

- JSON `quantity` → C++ `TradeSignal::requested_quantity`. Kept as `quantity`
  in JSON because that's the issue's word and what a strategy author would
  type; kept as `requested_quantity` in C++ because that field already
  exists and already means "one of two mutually exclusive sizing intents"
  (the other being `target_exposure` — §6). Renaming the C++ field was out of
  scope: it would touch every future reader of `TradeSignal` for a cosmetic
  reason.
- JSON `price` → C++ `TradeSignal::limit_price`. Kept as `price` in JSON to
  match the issue text; named `limit_price` in C++ to match `Order`'s own
  field of the same name, so the two structs read consistently side by side.

Everything else uses the same word in both.

### 3. Examples

Committed copies live in
[`../../tests/fixtures/signals/`](../../tests/fixtures/signals/)
(`new_order_market.json`, `new_order_limit.json`, `cancel.json`), reproduced
here for review. **These are proposed examples tied to this draft, not
golden files** — no JSON (de)serialization code exists in this repository
yet (§0, §9). They were checked for JSON syntax only, not against this
table.

**Prices below are plain decimal dollars (e.g. `152.75`), not scaled
integers — see §8.** That is a separate, independent question from ADR
0004's illustrative scaled-integer fixtures; neither is a ratified answer
to OQ#11.

**Market buy** (quantity example from the issue: 100 units):
```json
{
  "message_type": "new_order",
  "signal_id": 1042,
  "strategy_id": "sma_crossover",
  "symbol": "AAPL",
  "side": "buy",
  "order_type": "market",
  "quantity": 100,
  "created_at": "2026-09-16T14:32:01.123456789Z"
}
```

**Limit sell** (`price` present because `order_type` is `"limit"`):
```json
{
  "message_type": "new_order",
  "signal_id": 1043,
  "strategy_id": "sma_crossover",
  "symbol": "AAPL",
  "side": "sell",
  "order_type": "limit",
  "quantity": 50,
  "price": 152.75,
  "created_at": "2026-09-16T14:33:10.000000000Z"
}
```

**Cancel** (targets the market buy above, by its `signal_id`):
```json
{
  "message_type": "cancel",
  "signal_id": 1044,
  "target_signal_id": 1042,
  "strategy_id": "sma_crossover",
  "symbol": "AAPL",
  "created_at": "2026-09-16T14:34:00.000000000Z"
}
```

### 4. Why `message_type` isn't a C++ field

In JSON, one open object shape needs a tag to say how to read the rest of
it. In C++, `TradeSignal` and `SignalCancelRequest` are already two distinct
types — the type itself is the discriminator, the same way
`std::variant<TradeSignal, Order, ...>` in `event_bus.hpp` uses
`std::holds_alternative` / `std::visit` instead of a manual tag field. So
`message_type` exists only in the JSON documents, not in either struct.

### 5. ID ownership and traceability (the issue's open question)

Directly answering "should the same ID reach the Execution Simulator, or
should the Risk Manager assign another ID?", based on what the code already
does:

- **`SignalId` is minted exactly once**, by `StrategyEngine`, when a signal
  is created (`signal_ids_.next()` per the `TODO` in `strategy_engine.cpp`).
  It does not change afterward.
- **`RiskManager` mints no ID.** It has no `SequentialIdGenerator` member at
  all — by design, it evaluates the `SignalId` it's handed and returns a
  `RiskDecision` (approve / resize / reject); it does not relabel the
  signal.
- **`OrderId` is a separate id, minted separately**, by `ExecutionSimulator`,
  only after approval (`order_ids_.next()`), from its own independent
  `SequentialIdGenerator<OrderId>` counter. "Separate" describes the type and
  the counter, not a numeric range: `SignalId` and `OrderId` are each
  generated starting from 1, so a `SignalId` and an `OrderId` can perfectly
  well hold the same underlying number (e.g. both with `.value == 42`)
  without meaning the same thing or being confusable — `StrongId<OrderIdTag>`
  and `StrongId<SignalIdTag>` are unrelated template instantiations with no
  `operator==` between them at all, so the compiler rejects comparing one to
  the other directly. Identity comes from the type, never from the number.
- **The relationship is `Order::origin_signal : SignalId`**, which already
  exists in the code today as a field on the `Order` struct. This is how the
  *design* carries the original signal's identity through risk evaluation to
  the Execution Simulator — as a traceability field, not as the order's own
  identity. It is not yet something a running pipeline demonstrates:
  `RiskManager::check` and `ExecutionSimulator::submit` are both still
  `common::NotImplemented`, so no signal has actually flowed through this
  path and become an `Order` in this codebase yet. The scaffold's types are
  shaped to support that traceability once those two are implemented; they
  don't yet prove it happens. The order would get a new `OrderId` (rather
  than reusing the signal's) because a signal and the order(s) it produces
  are different-lifecycle entities — `order.hpp` already has a `TODO` for
  `parent_id for child slices`, meaning one signal may one day produce more
  than one order, which a single shared ID could not represent.

Net: this ADR does not change ID ownership. It documents what the existing
types already imply and proposes a matching JSON field, `signal_id`, at the
Strategy→Risk boundary — there is no `order_id` at that boundary, because the
order doesn't exist yet at that point in the pipeline.

### 6. Relationship to `SignalSide::Flat` and `target_exposure`

This ADR's `new_order` shape only covers explicit-quantity `buy`/`sell`
requests — it is **additive, not a replacement**:

- `SignalSide::Flat` and `TradeSignal::target_exposure` are untouched and
  still fully valid. A signal built the old way (e.g. `side = Flat`,
  `target_exposure = 0.0`, no `order_type`) is not reinterpreted by this ADR
  as anything else — it simply has the two new fields empty, like every
  field this ADR didn't touch.
- After this change, `TradeSignal` can describe **either** an exposure-based
  intent (pre-existing) **or** an explicit-quantity, order-shaped intent
  (this ADR) — never both, following the struct's existing "exactly one
  sizing intent" comment. This ADR does not add code enforcing that; it
  documents the new fields the same way `requested_quantity` /
  `target_exposure`'s exclusivity is already documented ("enforced later,
  not here").
- `Flat`, and how (or whether) it maps onto `order_type` / `price`, is
  explicitly **out of scope** here — issue #3's field list never mentions
  it. Left as a gap for whoever ratifies OQ#9.

### 7. Cancellation — differs from the issue's literal `side: "Cancel"`

Issue #3 lists `Side (Buy/Sell/Cancel)`. This ADR proposes something
narrower: a **separate message shape** (`message_type: "cancel"`), not a
third `side` value.

**This is a draft recommendation, not a settled choice** — the flat
`side: "cancel"` design the issue literally describes is a legitimate
alternative (see "Alternatives considered"). The reasoning for the split:
a cancel has no quantity, order type, or price; folding it into `side` means
every consumer has to know which fields to ignore based on another field's
value, whereas a separate shape makes combinations like
`side: "cancel", order_type: "limit"` not exist rather than needing a rule
against them. The existing code already treats cancellation as an operation
on something that exists (`OrderStatus::Cancelled` is a status transition),
never as a "side" — this shape follows that precedent.

#### Can a strategy actually obtain the `SignalId` it would need to cancel?

**Checked against the code: no, not currently.**
`strategy::ISignalSink::emit(const domain::TradeSignal&)` returns `void`
([`strategy.hpp`](../../include/trading_engine/strategy/strategy.hpp)). The
`TODO` in `strategy_engine.cpp` confirms the `SignalId` is stamped by the
engine *after* the strategy calls `emit()` — the strategy hands over a
signal with `id` still default/unset and never learns what id was assigned.
There is no callback such as `on_signal_acknowledged(SignalId)`, and
`IStrategy` has no hook for fills or order updates either (already a `TODO`
in `strategy.hpp`).

**This is a missing integration path, not something this ADR fixes.**
Closing it means either changing `ISignalSink::emit` to return the assigned
`SignalId`, or adding an acknowledgement callback to `IStrategy` — both are
runtime interface changes, explicitly out of scope for this draft. Until one
exists, a strategy cannot autonomously target one of its own earlier signals
for cancellation by id. (A human- or test-driven cancel, where the id is
already known from a log or a fixture, is unaffected by this gap.)

#### Cancellation target lifecycle — not decided, needs a policy

`ExecutionSimulator` has no cancellation entry point at all today
(`submit(TradeSignal)`, `submit(Order)`, `on_market_event(...)` are its only
public methods). So beyond what `target_signal_id` means at the JSON/struct
level, the following are genuinely open — recorded here, not answered:

| Target state | What should "cancel" mean? |
|---|---|
| Signal still awaiting a risk decision | Race: an order may be created before the cancel is even seen. Needs a policy (e.g. Risk Manager checks a pending-cancellation set before approving) — a pending signal is **not** automatically a safe no-op, since it can still become a real order. |
| Signal already rejected by Risk | Nothing to act on — is that a successful no-op, or should the caller be told "already resolved, no order existed"? |
| Signal approved, order Working / PartiallyFilled | The "obviously useful" case — but the mechanism (an `ExecutionSimulator::cancel(...)`-shaped method) doesn't exist yet. |
| Order already Filled | Too late — must not be reported as a successful cancel. |
| Order already Cancelled | Idempotent success, or an error ("already cancelled")? |
| Unknown `target_signal_id` (typo, wrong run, wrong strategy) | Must not silently succeed. |
| One signal produces multiple orders (future — `order.hpp` already has a `parent_id for child slices` TODO) | Does cancelling the signal cancel all of them, or must each order be targeted individually? |

None of these are implemented, routed, or looked up by this ADR's changes —
they're listed so the decision isn't silently made by omission later.

### 8. Numeric rules

- **Quantity** (`new_order.quantity`): must be finite and `> 0` in the JSON
  document. (JSON's grammar has no way to write `NaN`/`Infinity`, so this
  only matters once something reads the number into a C++ `double`, where
  those values are representable — nothing in `TradeSignal` currently
  rejects them.) Direction comes from `side`, matching `Order::quantity`'s
  existing "> 0, direction via side" convention, not from the sign of
  `quantity` itself — deliberately unlike `Fill::filled_quantity` and
  `Position::quantity`, which are signed further downstream (also noted from
  the execution side in ADR 0004 §4). That is an existing inconsistency
  across the domain model, not something this ADR introduces or resolves.

- **Whole shares are the proposed v1 policy — a change from this draft's
  earlier position, and still pending team approval.** The previous version
  of this section proposed *allowing* fractional quantities (citing Alpaca's
  fractional-share support) and asked reviewers to confirm or override that.
  [ADR 0004](https://github.com/amitathik7/CIS4914_Senior_Project/pull/7)
  §12 reports that the team's informal, in-chat direction leans
  whole-shares-only instead, and lists amending this section as the cheaper
  fix if that holds. **No ADR has ratified either answer** — ADR 0004 is
  explicit that it does not decide this — so rather than leave two Proposed
  drafts disagreeing, this section now aligns with that informal direction
  as the **proposed v1 policy, explicitly pending team approval**.

  - **Policy and representation are different questions.** `common::Quantity`
    stays `double` regardless of this policy — moving it is OQ#11's call
    (cross-cutting, all four members sign off), not this ADR's, and nothing
    here proposes it. A `double` can represent `100.5` whether or not the
    contract permits a signal to carry one; "whole shares" restricts which
    *values* are valid, not the field's type — and nothing in this codebase
    enforces that restriction today.
  - **This does not license silently rounding an explicit fractional
    request.** Under whole-shares-only, a caller asking for a fractional
    quantity is sending invalid input, not a value to floor without telling
    anyone. This ADR still proposes no runtime validation (§9), so nothing
    actually rejects it today either — but the *intended* handling, once
    something does, is an explicit rejection, not a silent truncation.
  - **Where a share count is computed rather than supplied directly**
    (sizing an exposure-based intent against a price — see §6), the proposed
    default for the resulting non-negative order magnitude is **rounding
    toward zero**, not half-away-from-zero or ceiling: rounding up can push
    an order past a risk limit the Risk Manager would otherwise have
    approved against. This mirrors the rounding direction ADR 0004 §12
    proposes on the execution side, so both ends round the same way. (Per
    that section, this sizing step happens in `ExecutionSimulator`, not in
    `StrategyEngine` or `RiskManager` — this ADR records the convention for
    consistency; it does not relocate where sizing happens.)
  - **A sizing result of zero shares must be an explicit, visible outcome,
    never a silent drop.** A small `target_exposure` against a high-priced
    symbol can floor to zero under whole-shares-only. This ADR does not
    implement that check, but proposes that whoever performs sizing must
    surface a zero result rather than quietly emitting nothing — consistent
    with ADR 0004 §14, which requires the equivalent order-sizing outcome to
    surface as an explicit `OrderStatus::Rejected` rather than vanish. Which
    component owns that check is not decided here.
  - This ADR still does **not** migrate `common::Quantity`, `Price`, or
    `Money` — see the OQ#11 note at the end of this section.

- **Price** (`new_order.price`): must be finite and `> 0` when present.
  Present **only** when `order_type == "limit"`; for `"market"` the key is
  **omitted entirely** (not `null`) — matching `Order::limit_price` being
  `std::optional`, "set iff type == Limit."

- **The units in §3's price examples are plain decimal, and that is still
  provisional — not a second scale competing with anyone else's.**
  `"price": 152.75` is an ordinary decimal number of dollars, no fixed-point
  scaling applied. ADR 0004's `fills/` fixtures instead show integers scaled
  by 10⁶ (`150020000` meaning `150.02`) as an illustration of one candidate
  encoding — explicitly flagged there as "illustrative, not ratified."
  Neither this ADR's decimal examples nor ADR 0004's scaled-integer examples
  are an accepted answer to OQ#11 (numeric representation — cross-cutting,
  all four members sign off); they simply show two different candidates in
  two sibling drafts. **`NUMERIC(18,6)` in the draft schema doesn't settle
  this either:** it is a column precision (up to six fractional decimal
  digits, stored exactly), not a JSON wire encoding — it neither requires
  nor implies a JSON number scaled by 10⁶, or an integer at all. Until
  OQ#11 lands, this ADR's price fields stay decimal, and if the team
  settles on a scale, this ADR's examples and
  `tests/fixtures/signals/*.json` need updating together, by hand — nothing
  makes that automatic.

- **A future cross-process binding needs more than a scale — flagged here,
  not added.** If Strategy→Risk ever crosses a Kafka topic (§0), at least
  two more things need coordinating across every contract sharing that
  topic space, not just this one: **run-scoped identity** (ADR 0004 §1 adds
  a `run_id` to `Fill` so a consumer can tell which backtest/live session a
  message belongs to; `TradeSignal` has no equivalent field) and **schema
  versioning** (ADR 0004 §11.4 proposes a `schema_version` field plus an
  additive-only change policy). Both are transport-binding concerns, not
  part of this ADR's logical contract, and this ADR does **not** add either
  field to `TradeSignal` or `SignalCancelRequest` now — doing so ahead of
  ADR 0003 would guess at a shape for a decision no one has made yet.
  Recorded so whoever writes ADR 0003 reconciles it with this contract too.

- **Correction to a claim from earlier discussion of this issue:** JSON's
  grammar (RFC 8259) does not mandate IEEE-754 double storage — it only
  defines the textual grammar for a number. IEEE-754 double is simply what
  many common parsers (e.g. JavaScript's `Number`, and JSON libraries that
  parse straight into `double`) happen to use internally, which is *why*
  very large integers can lose precision round-tripping through them — but
  that's a property of common implementations, not a requirement of JSON
  itself.
- **ID encoding (proposed, not settled):** this draft encodes `signal_id` /
  `target_signal_id` as JSON **numbers**, matching `StrongId::value`'s
  underlying `uint64_t`. Portability caveat: some JSON consumers parse all
  numbers as IEEE-754 double, which round-trips integers exactly only up to
  2^53. `SequentialIdGenerator` starts at 1 and increments by 1 per call, so
  reaching that range in one run isn't a realistic concern at this project's
  scale — but it would become one if IDs were ever generated a different way
  (random 64-bit, timestamp-based) or persisted and accumulated over a very
  long-lived system. Encoding IDs as JSON **strings** instead (as, e.g.,
  Discord/Twitter "snowflake" ids do) is a safer, equally simple alternative
  with no real cost, since ids are opaque to consumers — reviewers may
  prefer it.
- **This contract's number types are not independent of OQ#11.** `Money`,
  `Price`, and `Quantity` are `double` today, explicitly flagged provisional
  (`types.hpp`; ADR 0001 item 10) pending the numeric-representation ADR
  `OPEN_QUESTIONS.md` calls for. If that ADR moves these to a fixed-point or
  integer-minor-unit type, this contract's `quantity`/`price` JSON types
  (and any conversion code written once real (de)serialization exists) must
  be revisited by hand — nothing about this proposal makes that migration
  automatic.

### 9. Scope exclusions (explicitly not part of this change)

- No JSON library added anywhere, no (de)serialization code, no Kafka
  client, and no transport decision made. See §0. ADR 0003, if and when
  it's written, is a separate decision this ADR consumes, not one it makes.
- No changes to `event_bus.hpp`, `EventPayload`, `EventType`, `ISignalSink`,
  or `IStrategy`. The `SignalCancelRequest` type this ADR proposes cannot
  currently be emitted by a strategy or carried by the bus — see §7's
  integration-path note. Wiring that is future work with its own review.
- No behavior in `StrategyEngine`, `RiskManager`, or `ExecutionSimulator` —
  all three remain `common::NotImplemented` stubs, unchanged by this ADR.
- No cancellation routing, lookup, or lifecycle handling — §7's table stays
  unanswered by this ADR.
- No runtime validation code (no `validate(TradeSignal)`, no constructors
  that reject bad input). The numeric and required-field rules in §8 are
  documented rules, mirroring how `requested_quantity`/`target_exposure`'s
  mutual exclusivity is already documented rather than enforced.
- Does not touch `Money`/`Price`/`Quantity`, and does not resolve OQ#11.
- Does not ratify whole-shares-only vs. fractional quantities, or the
  price/quantity numeric scale — both stay proposed, pending OQ#11 and team
  sign-off (§8).
- Does not add `run_id`, `schema_version`, or any other transport-binding-
  only field to `TradeSignal` or `SignalCancelRequest` ahead of ADR 0003
  (§8).
- Does not resolve OQ#9 (signal semantics) — this ADR is additive alongside
  it, not a replacement.

## Consequences

**Positive**

- Gives issue #3 a concrete, reviewable shape without pre-committing to a
  JSON library, a transport, or any of M4–M6's actual behavior.
- Keeps `TradeSignal` able to express both existing signal styles
  (exposure-based) and the new explicit-quantity/order-shaped style, without
  redefining what any existing signal means.
- `Order::origin_signal` remains the one place this traceability is
  expressed in the type system; nothing about ID ownership changes. (This
  ADR doesn't implement or demonstrate that traceability at runtime — see
  §5.)
- Documents, in one place, a real integration gap (strategies can't learn
  their own assigned `SignalId`) that would otherwise surface later,
  mid-implementation, in M4 or M6.
- Reconciles §0's transport reading and §8's quantity policy with ADR
  0004's independently-drafted proposals (§0.3, §12) instead of leaving two
  Proposed ADRs quietly disagreeing.

**Negative / risks**

- Two message shapes (vs. one flat one) is a slightly bigger surface than
  the issue's literal wording — a reviewer may reasonably prefer the flatter
  shape for a smaller diff; see "Alternatives considered."
- `SignalCancelRequest` is currently a documented type with no way to reach
  any running component — it could look like dead code until the
  integration gap in §7 is closed. Kept anyway because the issue explicitly
  asks for the cancellation shape to be defined.
- Leaves several real questions open by design (cancellation lifecycle,
  `Flat` mapping, ID encoding, fractional-quantity policy) rather than
  resolving them — this is an architecture *proposal*, not a full spec, so
  issue #3 likely can't close on this ADR alone.

## Alternatives considered

- **Flat `side` including `"cancel"`, one message shape** — matches the
  issue text most literally, smaller diff (no `message_type`, no new
  struct). Rejected *for this draft* because it makes `quantity` /
  `order_type` / `price` mean "ignore me" under `side: "cancel"`, which is
  easy to get wrong; kept as the explicit fallback if reviewers prefer the
  smaller diff.
- **A distinct `CancelRequestId` type for `SignalCancelRequest::id`**,
  instead of reusing `SignalId` — more consistent with the existing
  one-tag-per-concept `StrongId` pattern (`OrderId`/`SignalId`/`FillId` are
  already kept apart specifically to prevent this kind of mix-up). Not
  chosen for this draft, to keep the diff smaller; a one-line change if
  reviewers prefer it.
- **JSON Schema file instead of a Markdown table** — more machine-checkable,
  but OQ#7 explicitly leaves "hand-rolled vs JSON Schema vs a library" as a
  separate, unresolved decision. Plain Markdown + example files avoids
  pre-empting that.
- **Encoding IDs as JSON strings** — safer against the 2^53 precision cliff,
  no real downside since IDs are opaque. Not chosen as the default in this
  draft, only to stay closest to `StrongId::value`'s native integer type;
  flagged in §8 as a reviewer-settleable swap.

## Sign-off

Unticked — this ADR is a draft prepared for review, not an accepted
decision.

- [ ] Strategy/risk area owner (Member C)
- [ ] Execution area owner (Member D)
- [ ] Transport/encoding split (§0) confirmed as the right approach —
      logical contract here vs. a transport-binding section added once
      ADR 0003 exists
- [ ] Whole-shares-only v1 policy (§8) confirmed or rejected, consistently
      with ADR 0004 §12 and issues #4/#5
- [ ] Rounding-toward-zero default for sizing (§8) confirmed, and its
      owning component named
- [ ] Price/quantity scale (§8, OQ#11) resolved and this ADR's examples plus
      `tests/fixtures/signals/*.json` updated together, by hand
- [ ] `docs/adr/README.md` index updated (done as part of this draft;
      re-confirm on acceptance)
- [ ] `docs/OPEN_QUESTIONS.md` OQ#9 updated (done as part of this draft;
      re-confirm on acceptance)
- [ ] Status line above changed from Proposed to Accepted (or
      Rejected / Superseded)
