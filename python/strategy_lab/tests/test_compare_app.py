"""The Compare view, driven through Streamlit's AppTest against the REAL strategy_lab_replay.

An automated check of the script and its widgets, not a browser. Every interaction is counted at the lowest level (each process the
bridge starts), so "this click did not run the engine" is proven for whichever view started it.

Hand-derived expectations used here (closes 3 2 1 2 3 4 3 2 1, one bar per minute, AAPL):
  SMA 2/3  (A): Buy on bar 5 (event_index 4), Sell on bar 8 (7)          - docs/strategies/moving_average_crossover.md section 5
  SMA 1/2  (B): bar 2 is the baseline (short 2 < long 2.5); bar 4 short 2 > long 1.5 Buy (event_index 3); bar 7 short 3 < long 3.5
                Sell (event_index 6). Both runs count their signal ids 1, 2: the ids REPEAT.
  mean reversion 4/1.5/0.5 (B): |z| never reaches 1.5 (max 1.414), so no request.
"""

import json
import unittest

from support import APP, ProcessCounter, VALID_CSV, chart_spec, open_view, page_text, real_runner

from streamlit.testing.v1 import AppTest


class CompareCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = real_runner()

    def setUp(self):
        self.counter = ProcessCounter()
        self.counter.__enter__()
        self.addCleanup(self.counter.__exit__)
        self.at = AppTest.from_file(APP, default_timeout=90).run()
        open_view(self.at, "Compare")

    def click(self, key):
        self.at.button(key=key).click().run()
        self.assertFalse(self.at.exception, [e.value for e in self.at.exception])

    def run_comparison(self):
        self.click("cmp_run")

    def choose(self, key, value):
        self.at.selectbox(key=key).set_value(value).run()
        self.assertFalse(self.at.exception, [e.value for e in self.at.exception])

    def number(self, member, kind, name, value):
        self.at.number_input(key=f"cmp_{member}_param.{kind}.{name}").set_value(value).run()

    @property
    def shown(self):
        return self.at.session_state["cmp_displayed"]

    @property
    def cursor(self):
        return self.at.session_state["cmp_cursor"]

    def signals(self, member):
        return [(s.side, s.event_index, s.signal_id) for s in self.shown.run_of(member).document.signals]


class FirstVisit(CompareCase):
    def test_nothing_runs_until_run_comparison_is_pressed(self):
        self.assertEqual(self.counter.runs, 0)
        self.assertNotIn("cmp_displayed", self.at.session_state)
        self.assertIn("press **Run comparison**", page_text(self.at))
        self.assertFalse(self.at.button(key="cmp_run").disabled)

    def test_the_default_pair_is_sma_versus_mean_reversion_on_one_shared_dataset_and_allowlist(self):
        self.assertEqual((self.at.selectbox(key="cmp_a_kind").value, self.at.selectbox(key="cmp_b_kind").value),
                         ("sma_crossover", "mean_reversion"))
        self.assertEqual(self.at.text_input(key="cmp_symbols").value, "AAPL")
        self.assertEqual(self.at.selectbox(key="cmp_builtin_key").value, "sma_crossover")
        self.assertEqual(self.at.number_input(key="cmp_a_param.sma_crossover.short_window").value, 2)
        self.assertEqual(self.at.number_input(key="cmp_b_param.mean_reversion.lookback").value, 4)

    def test_the_quick_start_for_two_variants_sets_two_sma_configurations(self):
        self.click("cmp_quick_two_sma")
        self.assertEqual((self.at.selectbox(key="cmp_a_kind").value, self.at.selectbox(key="cmp_b_kind").value),
                         ("sma_crossover", "sma_crossover"))
        self.assertEqual((self.at.number_input(key="cmp_a_param.sma_crossover.short_window").value,
                          self.at.number_input(key="cmp_a_param.sma_crossover.long_window").value), (2, 3))
        self.assertEqual((self.at.number_input(key="cmp_b_param.sma_crossover.short_window").value,
                          self.at.number_input(key="cmp_b_param.sma_crossover.long_window").value), (1, 2))
        self.assertEqual(self.counter.runs, 0, "loading a quick start only edits settings")


class SmaVersusMeanReversion(CompareCase):
    def test_one_run_starts_exactly_two_independent_replays_with_the_documented_result(self):
        self.run_comparison()
        self.assertEqual(self.counter.runs, 2)
        self.assertEqual(self.at.session_state["cmp_launches"], 1)
        self.assertEqual(self.signals("A"), [("buy", 4, 1), ("sell", 7, 2)])
        self.assertEqual(self.signals("B"), [])
        a, b = self.shown.runs
        self.assertEqual(a.document.dataset.sha256, b.document.dataset.sha256)
        self.assertNotEqual(a.document.run_id, b.document.run_id)
        text = page_text(self.at)
        self.assertIn("A: Moving-average crossover (short_window 2, long_window 3)", text)
        self.assertIn("lookback 4, entry_threshold 1.5, rearm_threshold 0.5", text)
        self.assertIn("full run: 2 signal requests (1 buy, 1 sell)", text)
        self.assertIn("full run: 0 signal requests (0 buy, 0 sell)", text)
        self.assertNotIn("These settings have changed", text)

    def test_both_charts_are_one_figure_with_the_same_event_sequence(self):
        self.run_comparison()
        self.click("cmp_nav_signal")
        self.assertEqual(self.cursor, 5)
        figure = chart_spec(self.at)
        titles = [a["text"] for a in figure["layout"]["annotations"] if " - " in a["text"] or ":" in a["text"]]
        self.assertTrue(any(t.startswith("A: sma_crossover") for t in titles), titles)
        self.assertTrue(any(t.startswith("B: mean_reversion") and t.endswith("z-score") for t in titles), titles)
        closes = [t for t in figure["data"] if t["name"] == "Close"]
        self.assertEqual(len(closes), 2, "one close line per configuration panel")
        self.assertEqual(closes[0]["x"], closes[1]["x"], "both panels are drawn on the same time sequence")
        self.assertEqual(len(closes[0]["x"]), 5)
        buys = [t for t in figure["data"] if t["name"] == "Buy request"]
        self.assertEqual(len(buys), 1, "only A requested a Buy; the shared legend entry is not duplicated")
        self.assertEqual(buys[0]["text"], ["Buy"])

    def test_going_back_removes_the_future_from_both_sides(self):
        self.run_comparison()
        for _ in range(2):
            self.click("cmp_nav_signal")
        self.assertEqual(self.cursor, 8)
        for _ in range(3):
            self.click("cmp_nav_prev")
        self.assertEqual(self.cursor, 5)
        figure = chart_spec(self.at)
        self.assertEqual([t["name"] for t in figure["data"]].count("Sell request"), 0)
        for trace in figure["data"]:
            if trace["name"] in ("Close", "z-score") or trace["name"].startswith("B: z"):
                self.assertLessEqual(len(trace["x"]), 5, trace["name"])
        text = page_text(self.at)
        self.assertIn("Event 5 of 9", text)
        counts = self.at.get("table")[2].value             # the counts table: Item | A | B
        self.assertEqual(counts.iloc[0, 1:].tolist(), [5, 5])               # events revealed, both sides
        self.click("cmp_nav_reset")
        self.assertEqual(self.chart_traces(), 0)

    def chart_traces(self):
        return len(chart_spec(self.at)["data"])

    def test_presentation_interactions_never_start_the_engine(self):
        self.run_comparison()
        self.assertEqual(self.counter.runs, 2)
        for action in ("cmp_nav_next", "cmp_nav_next", "cmp_nav_prev", "cmp_nav_signal", "cmp_nav_reset"):
            self.click(action)
        self.at.slider(key="cmp_slider").set_value(6).run()
        self.choose("cmp_symbol", "AAPL")
        self.choose("cmp_symbol", self.shown.model.all_label)
        for button in self.at.get("download_button"):
            button.click().run()
        self.number("a", "sma_crossover", "short_window", 1)               # editing settings is not running either
        open_view(self.at, "Explore")
        open_view(self.at, "Compare")
        self.assertEqual(self.counter.runs, 2)
        self.assertEqual(self.at.session_state["cmp_launches"], 1)

    def test_the_downloads_say_whether_they_cover_the_full_run_or_the_visible_prefix(self):
        self.run_comparison()
        labels = [b.proto.label for b in self.at.get("download_button")]
        self.assertTrue(any("Signals CSV - VISIBLE PREFIX" in label for label in labels), labels)
        self.assertTrue(any("Summary CSV - VISIBLE PREFIX" in label for label in labels), labels)
        self.assertTrue(any("comparison package - FULL RUN" in label for label in labels), labels)


class TwoVariantsOfOneStrategy(CompareCase):
    def setUp(self):
        super().setUp()
        self.click("cmp_quick_two_sma")
        self.run_comparison()

    def test_the_documented_signals_of_both_variants(self):
        self.assertEqual(self.signals("A"), [("buy", 4, 1), ("sell", 7, 2)])
        self.assertEqual(self.signals("B"), [("buy", 3, 1), ("sell", 6, 2)])

    def test_raw_signal_ids_repeat_across_the_runs_and_the_scoped_references_do_not(self):
        raw_ids = sorted((m, i) for m in ("A", "B") for _, _, i in self.signals(m))
        self.assertEqual(raw_ids, [("A", 1), ("A", 2), ("B", 1), ("B", 2)])
        refs = [s.member_ref for s in self.shown.model.signals_through(self.shown.model.total)]
        self.assertEqual(len(set(refs)), 4)

    def test_the_combined_next_signal_visits_each_event_where_either_signalled(self):
        stops = []
        while not self.at.button(key="cmp_nav_signal").disabled:
            self.click("cmp_nav_signal")
            stops.append(self.cursor)
        self.assertEqual(stops, [4, 5, 7, 8])                   # B at events 4 and 7 (1-based), A at 5 and 8

    def test_the_selected_event_shows_what_each_side_recorded(self):
        self.click("cmp_nav_signal")                            # event 4: only B has a request
        text = page_text(self.at)
        self.assertIn("Event 4 of 9", text)
        self.assertIn("Only B produced a signal request here (Buy request)", text)
        self.click("cmp_nav_signal")                            # event 5: only A
        self.assertIn("Only A produced a signal request here (Buy request)", page_text(self.at))

    def test_editing_one_variant_marks_only_that_side_stale_and_launches_nothing(self):
        before = self.counter.runs
        self.number("b", "sma_crossover", "long_window", 4)
        text = page_text(self.at)
        self.assertIn("These settings have changed since the displayed comparison", text)
        self.assertIn("B: parameters (long_window)", text)
        self.assertNotIn("A: parameters", text)
        self.assertEqual(self.counter.runs, before)
        self.assertEqual(dict(self.shown.request.members[1].params)["long_window"], "2")        # the snapshot did not change

    def test_putting_the_setting_back_makes_the_comparison_current_again(self):
        self.number("b", "sma_crossover", "long_window", 4)
        self.number("b", "sma_crossover", "long_window", 2)
        self.assertNotIn("These settings have changed", page_text(self.at))

    def test_identical_configurations_are_allowed_and_say_so(self):
        self.number("b", "sma_crossover", "short_window", 2)
        self.number("b", "sma_crossover", "long_window", 3)
        self.run_comparison()
        self.assertIn("The two configurations are identical", page_text(self.at))
        a, b = self.shown.runs
        self.assertEqual(a.document.run_id, b.document.run_id)                       # same content hash...
        refs = [s.member_ref for s in self.shown.model.signals_through(self.shown.model.total)]
        self.assertEqual(len(set(refs)), len(refs))                                   # ...yet every key is still unique


class FailureHandling(CompareCase):
    def make_b_invalid(self):
        self.number("b", "mean_reversion", "lookback", 1)

    def test_when_only_b_fails_the_message_names_b_and_nothing_is_paired(self):
        self.make_b_invalid()
        self.run_comparison()
        self.assertEqual(self.counter.runs, 2, "both configurations were attempted")
        failure = self.at.session_state["cmp_failure"]
        self.assertEqual(list(failure.errors), ["B"])
        self.assertEqual(failure.succeeded, ("A",))
        text = page_text(self.at)
        self.assertIn("No comparison was produced", text)
        self.assertIn("B: Mean reversion", text)
        self.assertIn("lookback must be at least 2", text)                               # the tool's own words
        self.assertIn("its own replay finished, but a comparison needs both", text)
        self.assertNotIn("cmp_displayed", self.at.session_state, "A's result alone is never presented as a comparison")

    def test_a_failed_run_keeps_the_last_completed_comparison_and_labels_it(self):
        self.run_comparison()
        first = self.shown
        self.make_b_invalid()
        self.run_comparison()
        self.assertIs(self.shown, first)
        self.assertEqual(self.shown.number, 1)
        self.assertEqual(self.counter.runs, 4)
        text = page_text(self.at)
        self.assertIn("last completed comparison", text)
        self.assertIn("comparison 1", text)
        self.assertEqual({r.config.member for r in self.shown.runs}, {"A", "B"})
        self.assertEqual(self.signals("A"), [("buy", 4, 1), ("sell", 7, 2)], "still the old pair, both from run 1")

    def test_a_later_success_clears_the_failure_and_replaces_the_pair_as_a_unit(self):
        self.run_comparison()
        self.make_b_invalid()
        self.run_comparison()
        self.click("cmp_restore")
        self.run_comparison()
        self.assertIsNone(self.at.session_state["cmp_failure"])
        self.assertEqual(self.shown.number, 3)
        self.assertEqual({r.config.member for r in self.shown.runs}, {"A", "B"})

    def test_when_both_fail_both_are_named(self):
        self.make_b_invalid()
        self.at.selectbox(key="cmp_a_kind").set_value("mean_reversion").run()
        self.number("a", "mean_reversion", "lookback", 1)
        self.run_comparison()
        self.assertEqual(sorted(self.at.session_state["cmp_failure"].errors), ["A", "B"])
        text = page_text(self.at)
        self.assertIn("A: Mean reversion", text)
        self.assertIn("B: Mean reversion", text)

    def test_a_dataset_the_tool_rejects_fails_both_sides_with_the_line_and_column(self):
        self.at.radio(key="cmp_dataset_mode").set_value("Upload CSV").run()
        bad = b"symbol,exchange_time,type,price\nXYZ,2026-03-02T14:30:00Z,bar,abc\n"
        self.at.file_uploader(key="cmp_upload").upload("bad.csv", bad, "text/csv").run()
        self.run_comparison()
        failure = self.at.session_state["cmp_failure"]
        self.assertEqual(sorted(failure.errors), ["A", "B"])
        self.assertEqual((failure.errors["A"].problems[0].line, failure.errors["A"].problems[0].column), (2, "price"))


class DatasetsAndFilters(CompareCase):
    def test_a_two_symbol_dataset_with_equal_timestamps_keeps_alignment_under_the_filter(self):
        self.choose("cmp_builtin_key", "two_symbols_interleaved")
        self.assertEqual(self.at.text_input(key="cmp_symbols").value, "AAPL, MSFT")
        self.run_comparison()
        model = self.shown.model
        self.assertEqual(model.symbols, ("AAPL", "MSFT"))
        self.assertEqual(model.events[0].exchange_time, model.events[1].exchange_time)
        self.choose("cmp_symbol", "MSFT")
        self.assertEqual(self.cursor, 0, "the filter does not move the cursor")
        self.click("cmp_nav_signal")
        self.assertEqual(self.cursor, model.next_signal(0, "MSFT"))
        self.assertEqual(model.events[self.cursor - 1].symbol, "MSFT")
        titles = [a["text"] for a in chart_spec(self.at)["layout"]["annotations"] if ":" in a["text"]]
        self.assertTrue(titles and all("MSFT" in t for t in titles), titles)
        self.assertEqual(self.counter.runs, 2)

    def test_an_uploaded_dataset_is_the_one_both_configurations_ran_on(self):
        self.at.radio(key="cmp_dataset_mode").set_value("Upload CSV").run()
        self.at.file_uploader(key="cmp_upload").upload("mine.csv", VALID_CSV, "text/csv").run()
        self.assertEqual(self.at.text_input(key="cmp_symbols").value, "XYZ")
        self.number("a", "sma_crossover", "short_window", 1)
        self.number("a", "sma_crossover", "long_window", 3)
        self.run_comparison()
        a, b = self.shown.runs
        self.assertEqual(a.document.dataset.sha256, b.document.dataset.sha256)
        self.assertEqual((self.shown.source.kind, self.shown.source.synthetic), ("upload", False))
        self.assertIn("provenance unknown", page_text(self.at).lower())

    def test_changing_the_dataset_marks_the_result_stale_and_loads_both_demo_presets(self):
        self.run_comparison()
        self.choose("cmp_builtin_key", "mr_rearm")
        self.assertIn("dataset (sma_crossover", page_text(self.at))
        self.assertEqual(self.at.text_input(key="cmp_symbols").value, "AAPL")
        self.assertEqual(self.counter.runs, 2)


class Wording(CompareCase):
    def test_no_trade_profit_or_ranking_language_anywhere_in_a_finished_comparison(self):
        self.run_comparison()
        self.click("cmp_nav_signal")
        # The replay tool's own notice says there are no fills or P&L (a negation). Everything else is the UI's wording.
        text = page_text(self.at).replace(self.shown.runs[0].document.notice, "").lower()
        self.assertIn("p&l", self.shown.runs[0].document.notice.lower())
        for word in ("profit", "winner", "better strategy", "outperform", "return", "p&l", "pnl"):
            self.assertNotIn(word, text, word)
        self.assertIn("not a measure of quality", text)
        self.assertIn("not trades", text)


if __name__ == "__main__":
    unittest.main()
