"""Compare, without Streamlit and without the executable: the pairing of two independent runs, the shared cursor and display filter,
member-scoped identities (signal ids repeat across independent engine runs), stale detection, and partial failure.

The documents are built by hand (support.make_document), so equal timestamps, repeated raw ids and opposing signals are exact inputs.
"""

import unittest
from unittest import mock

from support import RUN_ID, ev, make_document, outcome_of, parse

from strategy_lab_ui import bridge, compare, datasets
from strategy_lab_ui.compare import (CompareModel, CompareRequest, ComparisonFailure, MemberConfig, MemberRun)
from strategy_lab_ui.errors import LabUiError
from strategy_lab_ui.replay import ReplayModel


def config(member: str, kind: str = "sma_crossover", **params: str) -> MemberConfig:
    base = {"short_window": "2", "long_window": "3", "symbols": "AAPL,MSFT"}
    base.update(params)
    return MemberConfig(member, kind, tuple(base.items()), f"{member}: {kind} {params}", f"{member}: {kind}")


def run_of(member: str, specs, *, run_id: str = RUN_ID, kind: str = "sma_crossover", **document_kwargs) -> MemberRun:
    document = make_document(specs, run_id=run_id, kind=kind, **document_kwargs)
    outcome = outcome_of(document)
    return MemberRun(config(member, kind), outcome, ReplayModel(outcome.document))


# Two symbols, equal timestamps in the file: AAPL and MSFT at minute 30, 31, 32 (events 0..5).
def rows(aapl=None, msft=None):
    """Six rows, AAPL then MSFT at each minute. `aapl` and `msft` map (minute, symbol) to that row's signals."""
    aapl, msft = aapl or {}, msft or {}
    out = []
    for m in (30, 31, 32):
        out.append(ev("AAPL", m, 10 + m, signals=aapl.get((m, "AAPL"), ())))
        out.append(ev("MSFT", m, 20 + m, signals=msft.get((m, "MSFT"), ())))
    return out


class RepeatedRawIds(unittest.TestCase):
    """Signal ids count 1, 2, 3... in EVERY independent engine run, so the two members' ids collide by design."""

    def setUp(self):
        a = rows({(31, "AAPL"): [(0, 1, "buy")], (32, "AAPL"): [(0, 2, "sell")]})
        b = rows(None, {(30, "MSFT"): [(0, 1, "sell")], (32, "MSFT"): [(0, 2, "buy")]})
        self.model = CompareModel([run_of("A", a), run_of("B", b, run_id="fedcba9876543210")])

    def test_both_members_have_signal_ids_1_and_2_and_both_are_kept_raw(self):
        everything = self.model.signals_through(self.model.total)
        self.assertEqual(sorted((s.member, s.signal.signal_id) for s in everything),
                         [("A", 1), ("A", 2), ("B", 1), ("B", 2)])

    def test_the_scoped_key_distinguishes_them_and_carries_the_raw_run_reference(self):
        keys = [s.member_ref for s in self.model.signals_through(self.model.total)]
        self.assertEqual(len(set(keys)), 4)
        self.assertIn(f"A/{RUN_ID}:1", keys)
        self.assertIn("B/fedcba9876543210:1", keys)

    def test_identical_run_ids_are_still_told_apart_by_the_member(self):
        """Two identical configurations have the same content-addressed run_id; the member scope keeps them apart."""
        specs = rows({(31, "AAPL"): [(0, 1, "buy")]})
        model = CompareModel([run_of("A", specs), run_of("B", specs)])
        found = model.signals_through(model.total)
        self.assertEqual([s.signal.signal_ref for s in found], [f"{RUN_ID}:1", f"{RUN_ID}:1"])
        self.assertEqual([s.member_ref for s in found], [f"A/{RUN_ID}:1", f"B/{RUN_ID}:1"])

    def test_signals_are_never_joined_to_the_other_members_events_by_id(self):
        on_event_2 = self.model.signals_on(2)               # AAPL minute 31
        self.assertEqual([(s.member, s.signal.symbol, s.signal.side) for s in on_event_2], [("A", "AAPL", "buy")])
        on_event_1 = self.model.signals_on(1)               # MSFT minute 30
        self.assertEqual([(s.member, s.signal.symbol, s.signal.side) for s in on_event_1], [("B", "MSFT", "sell")])


class EachMemberReadsItsOwnResult(unittest.TestCase):
    def test_result_of_returns_that_members_recorded_decision_not_the_other_ones(self):
        """Regression: events are per document, so B's decision must be read from B's own event, never from A's event object."""
        a = rows()
        b = rows()
        a[3] = ev("MSFT", 31, 51, verdict="evaluated", reason="crossover_buy")
        b[3] = ev("MSFT", 31, 51, verdict="evaluated", reason="same_side")
        model = CompareModel([run_of("A", a), run_of("B", b)])
        event = model.events[3]
        self.assertEqual(model.result_of("A", event).reason, "crossover_buy")
        self.assertEqual(model.result_of("B", event).reason, "same_side")

    def test_the_counts_and_the_signals_of_one_member_never_include_the_other(self):
        a = rows({(30, "AAPL"): [(0, 1, "buy")]})
        b = rows()
        model = CompareModel([run_of("A", a), run_of("B", b)])
        counts = model.counts(model.total)
        self.assertEqual((counts["A"].buys, counts["A"].sells), (1, 0))
        self.assertEqual((counts["B"].buys, counts["B"].sells), (0, 0))


class SharedCursorAndFilter(unittest.TestCase):
    """AAPL and MSFT share every timestamp; the cursor counts EVENTS, so equal times stay distinct."""

    def setUp(self):
        a = rows({(31, "AAPL"): [(0, 1, "buy")]}, {(32, "MSFT"): [(0, 2, "sell")]})
        b = rows({(31, "AAPL"): [(0, 2, "sell")]}, {(30, "MSFT"): [(0, 1, "sell")]})
        self.model = CompareModel([run_of("A", a), run_of("B", b)])
        # event numbers (1-based) of the signals: A: AAPL 31 -> event 3, MSFT 32 -> event 6; B: MSFT 30 -> 2, AAPL 31 -> 3

    def test_the_combined_next_signal_stops_where_either_member_signalled(self):
        stops, cursor = [], 0
        while (cursor := self.model.next_signal(cursor)) is not None:
            stops.append(cursor)
        self.assertEqual(stops, [2, 3, 6])           # B at event 2; both at event 3 (ONE stop); A at event 6

    def test_simultaneous_signals_are_one_stop_and_both_are_listed(self):
        self.assertEqual(self.model.next_signal(2), 3)
        on_event = self.model.signals_on(2)          # index 2 = event 3
        self.assertEqual(sorted((s.member, s.signal.side) for s in on_event), [("A", "buy"), ("B", "sell")])

    def test_the_display_filter_applies_to_the_combined_stop(self):
        self.assertEqual(self.model.next_signal(0, "AAPL"), 3)
        self.assertEqual(self.model.next_signal(3, "AAPL"), None)
        self.assertEqual(self.model.next_signal(0, "MSFT"), 2)
        self.assertEqual(self.model.next_signal(2, "MSFT"), 6)

    def test_next_bar_and_previous_follow_the_filtered_symbol_and_keep_equal_times_apart(self):
        self.assertEqual(self.model.next_bar(0, "MSFT"), 2)           # the first MSFT row is event 2, same time as AAPL's event 1
        self.assertEqual(self.model.next_bar(2, "MSFT"), 4)
        self.assertEqual(self.model.previous_bar(4, "MSFT"), 2)
        self.assertEqual(self.model.events[0].exchange_time, self.model.events[1].exchange_time)

    def test_going_back_removes_future_requests_from_both_members(self):
        everything = self.model.counts(self.model.total)
        self.assertEqual((everything["A"].signals, everything["B"].signals), (2, 2))
        at_3 = self.model.counts(3)
        self.assertEqual((at_3["A"].signals, at_3["B"].signals), (1, 2))
        at_1 = self.model.counts(1)
        self.assertEqual((at_1["A"].signals, at_1["B"].signals), (0, 0))
        self.assertEqual([s.member for s in self.model.signals_through(2)], ["B"])

    def test_the_filter_limits_counts_to_that_symbol_for_both(self):
        msft = self.model.counts(self.model.total, "MSFT")
        self.assertEqual((msft["A"].buys, msft["A"].sells, msft["B"].sells), (0, 1, 1))

    def test_the_cursor_clamps_for_both_members(self):
        self.assertEqual(self.model.clamp(99), 6)
        self.assertEqual(self.model.clamp(-3), 0)


class WhatTheTwoRunsSayOnOneEvent(unittest.TestCase):
    def sentence(self, a, b):
        model = CompareModel([run_of("A", rows(a)), run_of("B", rows(b))])
        from strategy_lab_ui.compare_tables import agreement_sentence
        return agreement_sentence(model, 2)             # AAPL minute 31

    def test_neither(self):
        self.assertIn("Neither", self.sentence({}, {}))

    def test_both_same_side(self):
        text = self.sentence({(31, "AAPL"): [(0, 1, "buy")]}, {(31, "AAPL"): [(0, 1, "buy")]})
        self.assertIn("Both configurations requested Buy request", text)

    def test_opposing(self):
        text = self.sentence({(31, "AAPL"): [(0, 1, "buy")]}, {(31, "AAPL"): [(0, 1, "sell")]})
        self.assertIn("Opposing requests", text)
        self.assertIn("A Buy request", text)
        self.assertIn("B Sell request", text)

    def test_only_one_member(self):
        text = self.sentence({}, {(31, "AAPL"): [(0, 1, "sell")]})
        self.assertIn("Only B produced a signal request", text)
        self.assertIn("A produced none", text)


class Readiness(unittest.TestCase):
    """Readiness comes from the recorded window. A ready strategy may emit nothing; a signal is not needed to be 'ready'."""

    def test_a_full_window_with_no_signal_is_ready(self):
        specs = [ev("AAPL", 30, 1), ev("AAPL", 31, 2)]
        model = CompareModel([run_of("A", specs), run_of("B", specs)])
        found = model.readiness("A", 2)
        self.assertEqual([(r.symbol, r.state, r.fill, r.size, r.as_of_event) for r in found], [("AAPL", "ready", 3, 3, 2)])
        self.assertEqual(model.counts(2)["A"].signals, 0)

    def test_a_partial_window_is_warming_up_even_when_a_signal_exists_elsewhere(self):
        warm = {"fill": 2, "remaining": 1, "size": 3}
        a = [ev("AAPL", 30, 1, verdict="warming_up", reason="warming_up", window=warm, signals=[(0, 1, "buy")])]
        model = CompareModel([run_of("A", a), run_of("B", a)])
        state = model.readiness("A", 1)[0]
        self.assertEqual((state.state, state.fill, state.size), ("warming_up", 2, 3))

    def test_no_rows_yet_and_trade_rows_only(self):
        specs = [ev("AAPL", 30, 1), ev("MSFT", 30, 2, type="trade", verdict="ignored", reason="not_a_bar", window=None)]
        # a trade row reports no window fill: remove it from the recorded result
        document = make_document(specs)
        for event in document["result"]["events"]:
            if event["type"] == "trade":
                event["results"][0]["window"] = {"size": 3}
        outcome = outcome_of(document)
        run = MemberRun(config("A"), outcome, ReplayModel(outcome.document))
        model = CompareModel([run, run])
        states = {r.symbol: r.state for r in model.readiness("A", 2)}
        self.assertEqual(states, {"AAPL": "ready", "MSFT": "not_tracked"})
        self.assertEqual({r.symbol: r.state for r in model.readiness("A", 0)}, {"AAPL": "no_rows", "MSFT": "no_rows"})

    def test_the_filter_limits_readiness_to_one_symbol(self):
        model = CompareModel([run_of("A", rows()), run_of("B", rows())])
        self.assertEqual([r.symbol for r in model.readiness("B", 6, "MSFT")], ["MSFT"])


class OnlyComparableRunsAreCompared(unittest.TestCase):
    def pair(self, a_specs, b_specs, **b_kwargs):
        return [run_of("A", a_specs), run_of("B", b_specs, **b_kwargs)]

    def test_runs_over_different_dataset_bytes_are_refused(self):
        with self.assertRaises(LabUiError) as caught:
            CompareModel(self.pair(rows(), rows(), dataset_sha="cd" * 32))
        self.assertEqual(caught.exception.kind, "comparison_mismatch")
        self.assertIn("datasets differ", caught.exception.message)

    def test_runs_with_a_different_event_sequence_are_refused(self):
        other = rows()
        other[1] = ev("MSFT", 30, 999)
        with self.assertRaises(LabUiError) as caught:
            CompareModel(self.pair(rows(), other))
        self.assertIn("event 2 is not the same row", caught.exception.message)

    def test_runs_with_a_different_event_count_are_refused(self):
        with self.assertRaises(LabUiError) as caught:
            CompareModel(self.pair(rows(), rows()[:4]))
        self.assertIn("event counts differ", caught.exception.message)

    def test_a_comparison_has_exactly_two_members(self):
        with self.assertRaises(ValueError):
            CompareModel([run_of("A", rows())])


class StaleDetection(unittest.TestCase):
    def request(self, a=None, b=None, *, sha="d1", exe="e1", dataset="sma_crossover", symbols="AAPL"):
        return CompareRequest("builtin", dataset, sha, (config("A", symbols=symbols, **(a or {})),
                                                        config("B", kind="mean_reversion", symbols=symbols, **(b or {}))), exe)

    def test_identical_requests_have_no_differences(self):
        self.assertEqual(self.request().differences(self.request()), [])

    def test_a_change_in_one_member_names_that_member_only(self):
        reasons = self.request(a={"short_window": "1"}).differences(self.request())
        self.assertEqual(reasons, ["A: parameters (short_window)"])
        reasons = self.request(b={"long_window": "9"}).differences(self.request())
        self.assertEqual(reasons, ["B: parameters (long_window)"])

    def test_the_shared_allowlist_is_reported_once(self):
        self.assertEqual(self.request(symbols="AAPL,MSFT").differences(self.request()), ["shared symbols allowlist"])

    def test_dataset_content_and_executable_changes_are_reported(self):
        self.assertEqual(self.request(sha="d2").differences(self.request()), ["dataset contents"])
        self.assertIn("replay executable was rebuilt or replaced", self.request(exe="e2").differences(self.request()))
        self.assertTrue(self.request(dataset="other", sha="d2").differences(self.request())[0].startswith("dataset ("))

    def test_a_strategy_change_for_one_member_is_reported(self):
        now = CompareRequest("builtin", "k", "d1", (config("A"), config("B")), "e1")
        before = CompareRequest("builtin", "k", "d1", (config("A"), config("B", kind="mean_reversion")), "e1")
        self.assertEqual(now.differences(before), ["B: strategy (mean_reversion -> sma_crossover)"])


class ExecuteAndPartialFailure(unittest.TestCase):
    """Two independent runs; a comparison exists only if both succeed and describe the same events."""

    def setUp(self):
        self.source = datasets.DatasetSource("builtin", "k", "k.csv", "ab" * 32, 10, "p", True, ("AAPL",), b"x")
        self.runner = bridge.Runner(("fake",), "fake.exe", 1, 1, "e" * 64)
        self.members = [config("A"), config("B", kind="mean_reversion")]
        self.good = {"A": outcome_of(make_document(rows({(31, "AAPL"): [(0, 1, "buy")]}))),
                     "B": outcome_of(make_document(rows(), kind="mean_reversion"))}

    def fake(self, failing=()):
        calls = []

        def run_replay(runner, *, dataset, dataset_sha256, kind, params, **kw):
            member = "A" if kind == "sma_crossover" else "B"
            calls.append((member, kind, dict(params), dataset))
            if member in failing:
                raise failing[member]
            return self.good[member]

        return calls, run_replay

    def test_each_member_is_run_on_the_same_dataset_bytes_with_its_own_parameters(self):
        calls, fake = self.fake()
        with mock.patch.object(bridge, "run_replay", fake):
            done = compare.execute(self.runner, self.source, self.members, 1, "now")
        self.assertEqual([c[0] for c in calls], ["A", "B"])
        self.assertEqual({c[3] for c in calls}, {b"x"})
        self.assertEqual(calls[0][2]["short_window"], "2")
        self.assertEqual(done.source.data, b"")                      # the bytes are not retained, only the identity
        self.assertEqual([r.config.member for r in done.runs], ["A", "B"])

    def test_progress_is_reported_per_member_in_order(self):
        _, fake = self.fake()
        seen = []
        with mock.patch.object(bridge, "run_replay", fake):
            compare.execute(self.runner, self.source, self.members, 1, "now", lambda m, s: seen.append((m, s)))
        self.assertEqual(seen, [("A", "running"), ("A", "done"), ("B", "running"), ("B", "done")])

    def test_when_b_fails_the_failure_names_b_and_a_is_not_presented_as_a_comparison(self):
        error = LabUiError("runner_rejected", "lookback must be at least 2", exit_code=2)
        calls, fake = self.fake({"B": error})
        with mock.patch.object(bridge, "run_replay", fake), self.assertRaises(ComparisonFailure) as caught:
            compare.execute(self.runner, self.source, self.members, 1, "now")
        self.assertEqual(list(caught.exception.errors), ["B"])
        self.assertEqual(caught.exception.succeeded, ("A",))
        self.assertEqual(len(calls), 2)                              # both were attempted

    def test_when_both_fail_both_are_named(self):
        calls, fake = self.fake({"A": LabUiError("timeout", "slow"), "B": LabUiError("runner_rejected", "bad", exit_code=2)})
        with mock.patch.object(bridge, "run_replay", fake), self.assertRaises(ComparisonFailure) as caught:
            compare.execute(self.runner, self.source, self.members, 1, "now")
        self.assertEqual(sorted(caught.exception.errors), ["A", "B"])
        self.assertEqual(caught.exception.succeeded, ())

    def test_runs_that_cannot_be_paired_are_a_failure_of_the_pair(self):
        self.good["B"] = outcome_of(make_document(rows()[:4], kind="mean_reversion"))
        _, fake = self.fake()
        with mock.patch.object(bridge, "run_replay", fake), self.assertRaises(ComparisonFailure) as caught:
            compare.execute(self.runner, self.source, self.members, 1, "now")
        self.assertEqual(list(caught.exception.errors), [""])
        self.assertEqual(caught.exception.succeeded, ("A", "B"))
        self.assertEqual(caught.exception.errors[""].kind, "comparison_mismatch")


class Labels(unittest.TestCase):
    def test_a_label_shows_the_exact_settings_but_not_the_housekeeping_ones(self):
        from strategy_lab_ui.catalog import StrategySpec
        spec = StrategySpec("sma_crossover", "Moving-average crossover", "", "", True, "", (), (), (), (), ())
        label = compare.make_label("A", spec, "sma_crossover", [("strategy_id", "x"), ("short_window", "2"),
                                                                 ("long_window", "3"), ("requested_quantity", "1"), ("symbols", "AAPL")])
        self.assertEqual(label, "A: Moving-average crossover (short_window 2, long_window 3)")
        self.assertEqual(compare.make_short("B", spec, "sma_crossover"), "B: Moving-average crossover")


if __name__ == "__main__":
    unittest.main()
