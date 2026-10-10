import { describe, expect, it } from "vitest";
import { LabError } from "../api/lab";
import {
  alignmentProblems, buildCompareModel, compareSignalsThrough, differencePositions, memberSignalKey, nextDifference, nextSignalEither, ribbonFor, standingOf,
  type CompareModel,
} from "./compare";
import { agreementSentence, compareEvent, requestText } from "./compareEvent";
import { agreementPercent, readiness, readinessText, summarize, summarizeFull } from "./compareSummary";
import type { RequestSnapshot } from "./draft";
import { makeSnapshot } from "./snapshot";
import { makeDoc, at, type Spec } from "./testDoc";
import { OTHER_SHA, replayResponse, SHA } from "./testClient";
import { nineBars, ready, snap, warm } from "./testCompare";

// ---- helpers ------------------------------------------------------------------------------------------------------------

const modelOf = (a: Spec[], b: Spec[]): CompareModel => buildCompareModel(snap(a), snap(b));
const first = (specs: Spec[]) => snap(specs);

// ---- alignment ----------------------------------------------------------------------------------------------------------

describe("alignment: the two runs must be the same recorded events", () => {
  it("pairs two runs of the same events and keeps each side's own result", () => {
    const { a, b } = nineBars();
    const model = modelOf(a, b);
    expect(model.total).toBe(9);
    expect(model.rows.map((r) => r.relation)).toEqual([
      "not_comparable", "not_comparable", "not_comparable", "not_comparable", "differ", "differ", "agree", "differ", "agree",
    ]);
    // Side A requested at events 5 and 8, side B at event 6 (1-based); each side's signals come from its own document.
    expect(model.members.A.signals.map((s) => s.eventIndex)).toEqual([4, 7]);
    expect(model.members.B.signals.map((s) => s.eventIndex)).toEqual([5]);
    expect(model.rows[4]!.sides).toEqual({ A: "buy", B: "" });
    expect(model.rows[5]!.sides).toEqual({ A: "", B: "buy" });
  });

  it("refuses runs over different dataset bytes", () => {
    const { a, b } = nineBars();
    expect(() => buildCompareModel(snap(a), snap(b, { sha: OTHER_SHA }))).toThrowError(LabError);
    try {
      buildCompareModel(snap(a), snap(b, { sha: OTHER_SHA }));
    } catch (e) {
      expect((e as LabError).kind).toBe("comparison_mismatch");
      expect((e as LabError).message).toMatch(/datasets differ/);
    }
  });

  it("refuses a different number of events", () => {
    const { a, b } = nineBars();
    expect(alignmentProblems(snap(a).doc, snap(b.slice(0, 8)).doc)).toEqual(["the event counts differ (9 and 8)"]);
  });

  it("refuses a row that is not the same row, naming the event and the field (never matching by timestamp)", () => {
    const { a, b } = nineBars();
    const changed = b.map((s, i) => (i === 2 ? { ...s, price: "3.000001" } : s));
    expect(alignmentProblems(snap(a).doc, snap(changed).doc)).toEqual(["event 3 is not the same row in both runs (price: 3 and 3.000001)"]);
    const swapped = b.map((s, i) => (i === 5 ? { ...s, symbol: "MSFT" } : s));
    expect(alignmentProblems(snap(a).doc, snap(swapped, {}).doc)[0]).toMatch(/event 6 .*symbol: AAPL and MSFT/);
  });

  it.each(["sourceLine", "symbol", "time", "type", "price", "open", "high", "low", "volume"] as const)("refuses runs whose %s differs on a single row, and names it", (field) => {
    const base = snap(nineBars().a).doc;
    const changed = { ...base, events: base.events.map((e, i) => (i === 3 ? { ...e, [field]: field === "sourceLine" ? 99 : `${e[field] ?? "1"}7` } : e)) };
    expect(alignmentProblems(base, changed)).toEqual([`event 4 is not the same row in both runs (${field}: ${String(base.events[3]![field])} and ${String(changed.events[3]![field])})`]);
    expect(alignmentProblems(base, base)).toEqual([]);
  });

  it("refuses runs that were replayed in a different clock mode or order, or with a different bus fault", () => {
    const base = snap(nineBars().a).doc;
    const context = (name: string) => ({ ...base, context: base.context.map((c) => (c.name === name ? { ...c, text: `${c.text} (other)` } : c)) });
    expect(alignmentProblems(base, context("clock"))).toEqual(["the replay clock differs"]);
    expect(alignmentProblems(base, context("replay_order"))).toEqual(["the replay order differs"]);
    expect(alignmentProblems(base, { ...base, busFault: "reject-signals" })).toEqual(["the bus fault policy differs"]);
  });

  it("refuses two replays that ran on different executables, so one engine identity is never claimed for both", () => {
    const { a, b } = nineBars();
    const one = snap(a);
    const other = snap(b);
    const rebuilt = { ...other, meta: { ...other.meta, runner: { ...other.meta.runner, sha256: "d4".repeat(32) } } };
    expect(() => buildCompareModel(one, rebuilt)).toThrowError(/different replay executables/);
    expect(buildCompareModel(one, other).total).toBe(9);
  });

  it("does not require equal bus sequences: those are per-run bookkeeping, not event identity", () => {
    const { a, b } = nineBars();
    const shifted = makeDoc(b, { datasetSha: SHA, kind: "sma_crossover" }).replace(/"bus_sequence":(\d+)/g, (_m, n: string) => `"bus_sequence":${Number(n) + 100}`);
    const request: RequestSnapshot = { kind: "sma_crossover", params: [["strategy_id", "sma_crossover"]], dataset: { kind: "builtin", name: "x.csv", sha256: SHA } };
    const second = makeSnapshot(1, request, "Title", { ...replayResponse({ strategy: { kind: request.kind, params: request.params }, dataset: { builtin: "x" } }, SHA, b), raw: shifted }, "2026-10-09T12:00:00Z");
    expect(second.model.events[8]!.busSequence).toBe(first(a).model.events[8]!.busSequence + 100);
    expect(alignmentProblems(first(a).doc, second.doc)).toEqual([]);
    expect(buildCompareModel(first(a), second).total).toBe(9);
  });
});

// ---- identity: equal timestamps, interleaved symbols, repeated and huge ids ----------------------------------------------

describe("event identity and signal keys", () => {
  it("joins by recorded position, so equal timestamps on different symbols stay distinct events", () => {
    const t = at(5);
    const a: Spec[] = [
      { symbol: "AAPL", minute: 0, time: at(1) }, { symbol: "MSFT", minute: 0, time: at(1) },
      { symbol: "AAPL", minute: 1, time: t }, { symbol: "MSFT", minute: 1, time: t, signals: [[0, "1", "buy"]], reason: "crossover_buy" },
    ];
    const b: Spec[] = [
      { symbol: "AAPL", minute: 0, time: at(1) }, { symbol: "MSFT", minute: 0, time: at(1) },
      { symbol: "AAPL", minute: 1, time: t, signals: [[0, "1", "buy"]], reason: "crossover_buy" }, { symbol: "MSFT", minute: 1, time: t },
    ];
    const model = modelOf(a, b);
    // A timestamp join would pair both rows at time T and see one Buy on each side. By position, the sides asked on different rows.
    expect(model.rows[2]!.sides).toEqual({ A: "", B: "buy" });
    expect(model.rows[3]!.sides).toEqual({ A: "buy", B: "" });
    expect(model.rows.map((r) => r.requestsDiffer)).toEqual([false, false, true, true]);
    expect(differencePositions(model, "requests", "MSFT")).toEqual([4]); // only the MSFT row
    expect(differencePositions(model, "requests", "AAPL")).toEqual([3]);
    expect(differencePositions(model, "requests", null)).toEqual([3, 4]);
    expect(model.members.A.indexesBySymbol.get("MSFT")).toEqual([1, 3]); // recorded order is kept
  });

  it("scopes every signal by its side, because raw ids repeat across the two runs (and may share a run id)", () => {
    const { a, b } = nineBars(); // both sides' first request is raw id "1"; both documents carry run id 0123456789abcdef
    const model = modelOf(a, b);
    const ones = model.signals.filter((s) => s.signal.id === "1");
    expect(ones.map((s) => s.key)).toEqual(["A/0123456789abcdef:1", "B/0123456789abcdef:1"]);
    expect(new Set(model.signals.map((s) => s.key)).size).toBe(model.signals.length);
    expect(model.signals.map((s) => s.key)).toEqual(["A/0123456789abcdef:1", "B/0123456789abcdef:1", "A/0123456789abcdef:2"]); // by event, then side
  });

  it("keeps 64-bit signal ids exact in keys and ordering (two ids that are the same double stay different)", () => {
    expect(Number("9007199254740993")).toBe(Number("9007199254740992")); // the trap
    const a: Spec[] = [{ symbol: "AAPL", minute: 0, signals: [[0, "9007199254740993", "buy"]], reason: "crossover_buy" }, { symbol: "AAPL", minute: 1 }];
    const b: Spec[] = [{ symbol: "AAPL", minute: 0, signals: [[0, "9007199254740992", "buy"]], reason: "crossover_buy" }, { symbol: "AAPL", minute: 1 }];
    const model = modelOf(a, b);
    expect(model.signals.map((s) => s.key)).toEqual(["A/0123456789abcdef:9007199254740993", "B/0123456789abcdef:9007199254740992"]);
    expect(memberSignalKey("A", model.members.A.signals[0]!)).toBe("A/0123456789abcdef:9007199254740993");
    expect(model.rows[0]!.relation).toBe("agree"); // both asked for a Buy on the same row
  });

  it("orders two sides' requests on one event A before B, and ids by their digits", () => {
    const max = "9223372036854775807";
    const a: Spec[] = [{ symbol: "AAPL", minute: 0, signals: [[0, max, "buy"]], reason: "crossover_buy" }];
    const b: Spec[] = [{ symbol: "AAPL", minute: 0, signals: [[0, "1", "sell"]], reason: "crossover_sell" }];
    const model = modelOf(a, b);
    expect(model.signals.map((s) => [s.member, s.signal.id])).toEqual([["A", max], ["B", "1"]]);
    expect(model.rows[0]!.relation).toBe("differ"); // Buy against Sell
    expect(requestText(model.rows[0]!.sides.A)).toBe("Buy request");
  });
});

// ---- agreement and its denominator --------------------------------------------------------------------------------------

describe("agreement is stated over comparable events only", () => {
  it("counts a row as comparable only when both sides evaluated it, and says why the others were left out", () => {
    const { a, b } = nineBars();
    const full = summarizeFull(modelOf(a, b));
    expect(full.agreement).toMatchObject({ comparable: 5, agree: 2, differ: 3, notComparable: 4, requestDifferences: 3, verdictDifferences: 2 });
    expect(full.agreement.excluded).toEqual([{ label: "A evaluated · B warming up", count: 2 }, { label: "both warming up", count: 2 }]);
    expect(agreementPercent(full.agreement.agree, full.agreement.comparable)).toBe("40.0%");
    // Warming up is NOT an evaluated no-request: the two warm-up rows are in no denominator, and not in "agree".
    expect(full.agreement.agree + full.agreement.differ).toBe(full.agreement.comparable);
    expect(full.agreement.comparable + full.agreement.notComparable).toBe(full.events);
  });

  it("separates the visible prefix from the full run, and honours the display filter", () => {
    const { a, b } = nineBars();
    const model = modelOf(a, b);
    const prefix = summarize(model, 5, null);
    expect(prefix.events).toBe(5);
    expect(prefix.agreement).toMatchObject({ comparable: 1, agree: 0, differ: 1, notComparable: 4 });
    expect(prefix.members.A).toMatchObject({ buys: 1, sells: 0 });
    expect(prefix.members.B).toMatchObject({ buys: 0, sells: 0 }); // B's Buy is on event 6, not yet revealed
    expect(summarize(model, 0, null).agreement.comparable).toBe(0);
    expect(summarize(model, 9, "AAPL").events).toBe(9);
    expect(summarize(model, 9, "MSFT").events).toBe(0);
    expect(summarize(model, 3, null).members.A.verdicts).toMatchObject({ warming_up: 2, evaluated: 1 });
  });

  it("reports no ratio when nothing was comparable (insufficient warm-up), never 0% or 100%", () => {
    const warming = (n: number): Spec[] => Array.from({ length: n }, (_, i) => ({ symbol: "AAPL", minute: i, ...warm(i + 1, 50) }));
    const full = summarizeFull(modelOf(warming(6), warming(6)));
    expect(full.agreement).toMatchObject({ comparable: 0, notComparable: 6, agree: 0, differ: 0 });
    expect(agreementPercent(0, 0)).toBeUndefined();
    expect(full.agreement.excluded).toEqual([{ label: "both warming up", count: 6 }]);
  });

  it("two evaluated silent sides agree; a side with no diagnostics is never 'evaluated'", () => {
    const quiet: Spec[] = [{ symbol: "AAPL", minute: 0 }, { symbol: "AAPL", minute: 1 }];
    const nothing: Spec[] = [{ symbol: "AAPL", minute: 0 }, { symbol: "AAPL", minute: 1, available: false }];
    const model = modelOf(quiet, nothing);
    expect(model.rows.map((r) => r.relation)).toEqual(["agree", "not_comparable"]);
    expect(model.rows[1]!.standing).toEqual({ A: "evaluated", B: "unavailable" });
    expect(summarizeFull(model).agreement.excluded).toEqual([{ label: "A evaluated · B no diagnostics", count: 1 }]);
  });

  it("handles runs with no requests at all", () => {
    const quiet: Spec[] = Array.from({ length: 4 }, (_, i) => ({ symbol: "AAPL", minute: i }));
    const model = modelOf(quiet, quiet);
    expect(model.signals).toEqual([]);
    expect(nextSignalEither(model, 0, null)).toBeUndefined();
    expect(nextDifference(model, 0, null, "decisions")).toBeUndefined();
    expect(summarizeFull(model).agreement).toMatchObject({ comparable: 4, agree: 4, differ: 0 });
    expect(agreementPercent(4, 4)).toBe("100.0%");
  });

  it("formats the percentage with integer arithmetic", () => {
    expect(agreementPercent(1, 3)).toBe("33.3%");
    expect(agreementPercent(2, 3)).toBe("66.7%");
    expect(agreementPercent(1, 8)).toBe("12.5%");
    expect(agreementPercent(999, 1000)).toBe("99.9%");
    expect(agreementPercent(1, 6)).toBe("16.7%");
  });

  it("treats an unknown verdict as unavailable, not evaluated", () => {
    const { a } = nineBars();
    const model = modelOf(a, a);
    const r = model.members.A.events[0]!.results[0]!;
    expect(standingOf(r)).toBe("warming_up");
    expect(standingOf({ ...r, verdict: "mystery" })).toBe("unavailable");
    expect(standingOf({ ...r, available: false })).toBe("unavailable");
  });
});

// ---- the shared cursor and navigation -----------------------------------------------------------------------------------

describe("one shared cursor", () => {
  it("finds the next difference by request, or by request or verdict, from the recorded rows", () => {
    const { a, b } = nineBars();
    const model = modelOf(a, b);
    expect(differencePositions(model, "requests", null)).toEqual([5, 6, 8]);
    expect(differencePositions(model, "decisions", null)).toEqual([3, 4, 5, 6, 8]);
    expect([0, 5, 6, 8].map((c) => nextDifference(model, c, null, "requests"))).toEqual([5, 6, 8, undefined]);
    expect(nextDifference(model, 0, null, "decisions")).toBe(3);
    expect(nextDifference(model, 3, "AAPL", "decisions")).toBe(4);
    expect(nextDifference(model, 0, "MSFT", "requests")).toBeUndefined(); // no such symbol: nothing to stop at
  });

  it("stops at a request by EITHER side", () => {
    const { a, b } = nineBars();
    const model = modelOf(a, b);
    expect([0, 5, 6, 8].map((c) => nextSignalEither(model, c, null))).toEqual([5, 6, 8, undefined]);
  });

  it("lists both sides' requests in the visible prefix under the filter", () => {
    const { a, b } = nineBars();
    const model = modelOf(a, b);
    expect(compareSignalsThrough(model, 5, null).map((s) => s.key)).toEqual(["A/0123456789abcdef:1"]);
    expect(compareSignalsThrough(model, 6, null).map((s) => s.key)).toEqual(["A/0123456789abcdef:1", "B/0123456789abcdef:1"]);
    expect(compareSignalsThrough(model, 9, "MSFT")).toEqual([]);
  });

  it("builds a per-symbol ribbon of relations in recorded order", () => {
    const { a, b } = nineBars();
    expect(ribbonFor(modelOf(a, b), "AAPL")).toEqual(["not_comparable", "not_comparable", "not_comparable", "not_comparable", "differ", "differ", "agree", "differ", "agree"]);
  });
});

// ---- per-symbol readiness ----------------------------------------------------------------------------------------------

describe("per-symbol warm-up progress", () => {
  it("reads each side's recorded window per symbol, as of the last revealed row of that symbol", () => {
    const a: Spec[] = [
      { symbol: "AAPL", minute: 0, ...warm(1, 3) }, { symbol: "MSFT", minute: 0, ...warm(1, 3) },
      { symbol: "AAPL", minute: 1, ...warm(2, 3) }, { symbol: "MSFT", minute: 1, ...warm(2, 3) }, { symbol: "AAPL", minute: 2, ...ready(3) },
    ];
    const model = modelOf(a, a);
    expect(readiness(model, "A", 0, null).map((r) => r.state)).toEqual(["no_rows", "no_rows"]);
    expect(readiness(model, "A", 4, null).map(readinessText)).toEqual([
      "Warming up: 2 of 3 accepted bars (as of event 3)", "Warming up: 2 of 3 accepted bars (as of event 4)",
    ]);
    expect(readiness(model, "A", 5, null).map((r) => r.state)).toEqual(["ready", "warming_up"]); // AAPL got its third bar; MSFT did not
    expect(readiness(model, "A", 5, "MSFT")).toHaveLength(1);
  });

  it("does not infer readiness from requests, and says when no bar has reached the strategy", () => {
    const rows: Spec[] = [{ symbol: "AAPL", minute: 0, type: "trade", verdict: "ignored", reason: "not_a_bar", window: { size: 3 } }];
    const model = modelOf(rows, rows);
    expect(readiness(model, "A", 1, null)[0]).toMatchObject({ state: "not_tracked", asOfEvent: 1 });
    expect(readinessText(readiness(model, "A", 1, null)[0]!)).toBe("No bar has reached the strategy (as of event 1)");
  });

  it("shows the two sides' warm-up progress separately for one symbol (different windows)", () => {
    const { a, b } = nineBars();
    const model = modelOf(a, b);
    expect(readinessText(readiness(model, "A", 3, "AAPL")[0]!)).toBe("Ready: 3 of 3 accepted bars (as of event 3)");
    expect(readinessText(readiness(model, "B", 3, "AAPL")[0]!)).toBe("Warming up: 3 of 5 accepted bars (as of event 3)");
  });
});

// ---- the event inspector ------------------------------------------------------------------------------------------------

describe("event-level comparison", () => {
  it("flags differing decisions, reasons and readiness, and says so only where both sides reported", () => {
    const { a, b } = nineBars();
    const withIndicators = (specs: Spec[], value: string): Spec[] => specs.map((s, i) => (i === 4 ? { ...s, indicators: { short_sma: value }, unavailable: { z_score: "constant_window" } } : s));
    const model = modelOf(withIndicators(a, "2.5"), withIndicators(b, "3.5"));
    const rows = compareEvent(model, 4);
    const by = (label: string) => rows.find((r) => r.label === label)!;
    expect(by("Verdict")).toMatchObject({ a: "evaluated", b: "evaluated", status: "same" });
    expect(by("Reason code")).toMatchObject({ a: "crossover_buy", b: "same_side", status: "differs" });
    expect(by("Request")).toMatchObject({ a: "Buy request", b: "No request", status: "differs" });
    expect(by("Window").status).toBe("differs");
    expect(by("short_sma")).toMatchObject({ a: "2.5", b: "3.5", status: "differs" });
    expect(by("z_score").status).toBe("same"); // both: unavailable for the same reason
    expect(by("z_score").a).toMatch(/^unavailable: /);
  });

  it("does not call an indicator one side lacks a disagreement", () => {
    const a: Spec[] = [{ symbol: "AAPL", minute: 0, indicators: { short_sma: "1.5" } }];
    const b: Spec[] = [{ symbol: "AAPL", minute: 0, indicators: { mean: "1.5" } }];
    const rows = compareEvent(modelOf(a, b), 0);
    expect(rows.find((r) => r.label === "short_sma")).toMatchObject({ a: "1.5", b: "not reported by this strategy", status: "not_comparable" });
    expect(rows.find((r) => r.label === "mean")).toMatchObject({ a: "not reported by this strategy", b: "1.5", status: "not_comparable" });
  });

  it("describes what the two sides recorded without ranking them", () => {
    const { a, b } = nineBars();
    const model = modelOf(a, b);
    expect(agreementSentence(model, 0)).toMatch(/^Not comparable: both configurations were warming up/);
    expect(agreementSentence(model, 2)).toMatch(/^Not comparable: A was evaluated and B was warming up/);
    expect(agreementSentence(model, 4)).toBe("Only A made a request here (Buy request); B evaluated the row and made none.");
    expect(agreementSentence(model, 5)).toBe("Only B made a request here (Buy request); A evaluated the row and made none.");
    expect(agreementSentence(model, 6)).toMatch(/neither made a request: they agree/);
    const flip = modelOf([{ symbol: "AAPL", minute: 0, signals: [[0, "1", "buy"]], reason: "crossover_buy" }], [{ symbol: "AAPL", minute: 0, signals: [[0, "1", "sell"]], reason: "crossover_sell" }]);
    expect(agreementSentence(flip, 0)).toBe("Opposing or different requests on this row: A Buy request, B Sell request.");
    const same = modelOf([{ symbol: "AAPL", minute: 0, signals: [[0, "1", "buy"]], reason: "crossover_buy" }], [{ symbol: "AAPL", minute: 0, signals: [[0, "1", "buy"]], reason: "crossover_buy" }]);
    expect(agreementSentence(same, 0)).toMatch(/made the same request \(Buy request\): they agree/);
    expect(JSON.stringify([agreementSentence(model, 4), agreementSentence(model, 5)])).not.toMatch(/better|worse|win|best|profit/i);
  });
});
