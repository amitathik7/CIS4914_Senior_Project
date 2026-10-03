"""The Streamlit app driven by Streamlit's AppTest harness against the REAL strategy_lab_replay.

This is an automated check of the app script and its widgets, not a browser: there is no real DOM, canvas or
pointer here. (The browser checks done by hand are listed in docs/STRATEGY_LAB.md.) Every run starts the real
executable, so the tests are skipped, with a reason, if it has not been built.

A counter wraps the one function that starts a replay; tests assert it moves only when Run is pressed.
"""

import json
import unittest
from unittest import mock

from support import ROOT, real_runner

from streamlit.testing.v1 import AppTest

from strategy_lab_ui import bridge, datasets, session, state
from strategy_lab_ui.errors import LabUiError

APP = str(ROOT / "app.py")
VALID = (b"symbol,exchange_time,type,price\nXYZ,2026-03-02T14:30:00Z,bar,50\nXYZ,2026-03-02T14:31:00Z,bar,50\n"
         b"XYZ,2026-03-02T14:32:00Z,bar,50\nXYZ,2026-03-02T14:33:00Z,bar,40\nXYZ,2026-03-02T14:34:00Z,bar,60\n")


def page_text(at: AppTest) -> str:
    parts: list[str] = []
    for kind in ("markdown", "caption", "warning", "error", "info", "success", "text", "code"):
        parts += [str(e.value) for e in getattr(at, kind)]
    return "\n".join(parts)


class AppTestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = real_runner()

    def setUp(self):
        self.launches: list[tuple] = []
        original = state.run_replay

        def counting(*args, **kwargs):
            self.launches.append((kwargs.get("kind"), tuple(kwargs.get("params", ()))))
            return original(*args, **kwargs)

        patcher = mock.patch.object(state, "run_replay", counting)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.at = AppTest.from_file(APP, default_timeout=90)
        self.at.run()
        self.assertFalse(self.at.exception, [e.value for e in self.at.exception])

    # helpers -----------------------------------------------------------------------------------------
    def click(self, key):
        self.at.button(key=key).click().run()
        self.assertFalse(self.at.exception, [e.value for e in self.at.exception])

    def run_app(self):
        self.click("ui_run")

    def choose(self, key, value):
        self.at.selectbox(key=key).set_value(value).run()
        self.assertFalse(self.at.exception, [e.value for e in self.at.exception])

    def set_number(self, kind, name, value):
        self.at.number_input(key=f"ui_param.{kind}.{name}").set_value(value).run()

    def set_text(self, key, value):
        self.at.text_input(key=key).set_value(value).run()

    @property
    def cursor(self):
        return self.at.session_state["lab_cursor"]

    @property
    def displayed(self):
        return self.at.session_state["lab_displayed"]

    def chart(self):
        return json.loads(self.at.get("plotly_chart")[0].proto.spec)

    def badge_text(self):
        return " ".join(m.value for m in self.at.markdown if "-badge[" in m.value)

    def sma_demo(self):
        """Dataset sma_crossover, strategy sma_crossover, 2/3 AAPL: the app's default selection."""
        self.run_app()


class FirstLoad(AppTestCase):
    def test_nothing_runs_until_run_is_pressed(self):
        self.assertEqual(self.launches, [])
        self.assertEqual(self.at.session_state["lab_launches"], 0)
        self.assertNotIn("lab_displayed", self.at.session_state)
        self.assertIn("press **Run replay**", page_text(self.at))
        self.assertFalse(self.at.button(key="ui_run").disabled)

    def test_the_form_is_built_from_the_catalog_and_the_demo_preset(self):
        self.assertEqual(self.at.session_state["ui_dataset_mode"], "Built-in demo")
        self.assertEqual(self.at.selectbox(key="ui_builtin_key").value, "sma_crossover")
        self.assertEqual(self.at.selectbox(key="ui_strategy_kind").value, "sma_crossover")
        self.assertEqual((self.at.number_input(key="ui_param.sma_crossover.short_window").value,
                          self.at.number_input(key="ui_param.sma_crossover.long_window").value), (2, 3))
        self.assertEqual(self.at.text_input(key="ui_symbols").value, "AAPL")

    def test_the_ml_strategy_is_listed_as_unavailable_and_not_selectable(self):
        self.assertNotIn("ml", self.at.selectbox(key="ui_strategy_kind").options)
        self.assertIn("not available", page_text(self.at))

    def test_the_catalog_is_loaded_once_not_per_interaction(self):
        before = session.catalog_loads()
        self.run_app()
        self.click("nav_next")
        self.click("ui_restore")
        self.assertEqual(session.catalog_loads(), before)
        self.assertGreaterEqual(before, 1)

    def test_the_catalog_defaults_are_the_cpp_defaults(self):
        catalog = bridge.describe(self.runner)
        self.click("ui_restore")
        sma = catalog.strategy("sma_crossover")
        self.assertEqual(self.at.number_input(key="ui_param.sma_crossover.short_window").value, sma.param("short_window").default)
        self.assertEqual(self.at.number_input(key="ui_param.sma_crossover.long_window").value, sma.param("long_window").default)
        # A quantity is a whole number of shares (an int64): the catalog says "uint", so it is an integer input.
        self.assertEqual(self.at.number_input(key="ui_param.sma_crossover.requested_quantity").value, 1)
        self.assertEqual(self.at.text_input(key="ui_symbols").value, "AAPL")                  # the dataset's symbols


class BuiltinSma(AppTestCase):
    def test_run_and_walk_the_two_signals(self):
        self.sma_demo()
        self.assertEqual(len(self.launches), 1)
        self.assertEqual(self.launches[0][0], "sma_crossover")
        self.assertEqual(self.cursor, 0)
        self.assertIn("nothing revealed", page_text(self.at) + json.dumps(self.chart()["layout"]))
        self.assertEqual([(s.side, s.event_index) for s in self.displayed.document.signals], [("buy", 4), ("sell", 7)])

        self.click("nav_signal")
        self.assertEqual(self.cursor, 5)
        text = page_text(self.at)
        self.assertIn("Event 5 of 9", text)
        self.assertIn("crossover_buy", text)
        self.assertIn("Buy request #1", text)
        self.assertEqual([t["name"] for t in self.chart()["data"]].count("Buy request"), 1)

        self.click("nav_signal")
        self.assertEqual(self.cursor, 8)
        self.assertIn("crossover_sell", page_text(self.at))
        self.assertTrue(self.at.button(key="nav_signal").disabled)             # no signal left
        self.assertEqual(len(self.launches), 1)

    def test_stepping_forward_and_back_removes_the_future(self):
        self.sma_demo()
        self.click("nav_signal")
        self.assertEqual(len(self.chart()["data"][0]["x"]), 5)
        for _ in range(2):
            self.click("nav_prev")
        self.assertEqual(self.cursor, 3)
        names = [t["name"] for t in self.chart()["data"]]
        self.assertNotIn("Buy request", names)
        self.assertEqual(len(self.chart()["data"][0]["x"]), 3)
        self.click("nav_reset")
        self.assertEqual(self.cursor, 0)
        self.assertEqual(self.chart()["data"], [])

    def test_the_slider_moves_the_cursor_and_the_buttons_follow_it(self):
        self.sma_demo()
        self.at.slider(key="lab_slider").set_value(7).run()
        self.assertEqual(self.cursor, 7)
        self.assertIn("Event 7 of 9", page_text(self.at))
        self.click("nav_next")
        self.assertEqual((self.cursor, self.at.slider(key="lab_slider").value), (8, 8))

    def test_buttons_are_disabled_at_the_ends(self):
        self.sma_demo()
        self.assertTrue(self.at.button(key="nav_prev").disabled and self.at.button(key="nav_reset").disabled)
        self.at.slider(key="lab_slider").set_value(9).run()
        self.assertTrue(self.at.button(key="nav_next").disabled)
        self.assertFalse(self.at.button(key="nav_prev").disabled)

    def test_presentation_interactions_never_start_the_engine(self):
        self.sma_demo()
        for _ in range(3):
            self.click("nav_next")
        self.click("nav_prev")
        self.click("nav_signal")
        self.at.slider(key="lab_slider").set_value(6).run()
        self.click("nav_reset")
        self.choose("lab_symbol", "AAPL")
        self.choose("lab_symbol", self.displayed.model.all_label)
        self.at.get("download_button")[0].click().run()
        self.set_number("sma_crossover", "short_window", 1)          # editing settings is not running either
        self.assertEqual(len(self.launches), 1)
        self.assertEqual(self.at.session_state["lab_launches"], 1)

    def test_the_prefix_counters_follow_the_cursor(self):
        self.sma_demo()
        self.click("nav_signal")
        self.assertEqual(self.displayed.model.counts(self.cursor).buys, 1)
        self.click("nav_signal")
        counts = self.displayed.model.counts(self.cursor)
        self.assertEqual((counts.buys, counts.sells, counts.events), (1, 1, 8))

    def test_the_full_run_summary_does_not_depend_on_the_position(self):
        self.sma_demo()
        before = [m.value for m in self.at.markdown if "signal requests" in m.value]
        self.click("nav_signal")
        after = [m.value for m in self.at.markdown if "signal requests" in m.value]
        self.assertEqual(before, after)
        self.assertIn("9 events - 1 symbol(s) - 2 signal requests (1 buy, 1 sell)", before[0])


class StaleAndFailure(AppTestCase):
    def test_changing_a_setting_marks_the_result_as_belonging_to_the_previous_settings(self):
        self.sma_demo()
        snapshot = self.displayed.request
        self.assertNotIn("These settings have changed", page_text(self.at))
        self.set_number("sma_crossover", "short_window", 1)
        text = page_text(self.at)
        self.assertIn("These settings have changed since the displayed run", text)
        self.assertIn("short_window", text)
        self.assertIn("run 1", text)
        self.assertIs(self.displayed.request, snapshot)                       # the snapshot itself never changed
        self.assertEqual(dict(snapshot.params)["short_window"], "2")
        self.assertEqual(len(self.launches), 1)

    def test_putting_the_setting_back_makes_the_result_current_again(self):
        self.sma_demo()
        self.set_number("sma_crossover", "short_window", 1)
        self.set_number("sma_crossover", "short_window", 2)
        self.assertNotIn("These settings have changed", page_text(self.at))

    def test_changing_the_dataset_or_strategy_is_stale_too(self):
        self.sma_demo()
        self.choose("ui_builtin_key", "sma_oscillation")
        self.assertIn("dataset (sma_crossover", page_text(self.at))
        self.choose("ui_builtin_key", "sma_crossover")
        self.choose("ui_strategy_kind", "mean_reversion")
        self.assertIn("strategy (sma_crossover", page_text(self.at))

    def test_playback_position_survives_editing_but_a_new_run_starts_at_zero(self):
        self.sma_demo()
        self.click("nav_signal")
        self.set_number("sma_crossover", "short_window", 1)
        self.assertEqual(self.cursor, 5)
        self.run_app()
        self.assertEqual(self.cursor, 0)
        self.assertEqual(len(self.launches), 2)

    def test_a_failed_run_is_reported_and_the_last_success_stays_labelled(self):
        self.sma_demo()
        self.click("nav_signal")
        self.set_number("sma_crossover", "long_window", 1)
        self.set_number("sma_crossover", "short_window", 1)
        self.run_app()
        failure = self.at.session_state["lab_failure"]
        self.assertEqual(failure.error.kind, "runner_rejected")
        text = page_text(self.at)
        self.assertIn("long_window must be greater than short_window", text)          # the tool's own words
        self.assertIn("last successful run", text)
        self.assertEqual(self.displayed.number, 1)                                  # the retained result
        self.assertEqual(self.cursor, 5)                                            # and where the user was
        self.assertEqual(len(self.launches), 2)

    def test_a_later_success_clears_the_failure(self):
        self.sma_demo()
        self.set_number("sma_crossover", "long_window", 1)
        self.run_app()
        self.assertIsNotNone(self.at.session_state["lab_failure"])
        self.click("ui_restore")
        self.run_app()
        self.assertIsNone(self.at.session_state["lab_failure"])
        self.assertEqual(self.displayed.number, 3)


class MeanReversionAndEdges(AppTestCase):
    def test_mean_reversion_run_has_a_z_panel_with_the_configured_thresholds(self):
        self.choose("ui_builtin_key", "mr_rearm")
        self.choose("ui_strategy_kind", "mean_reversion")
        self.run_app()
        self.assertEqual([(s.side, s.event_index) for s in self.displayed.document.signals],
                         [("buy", 3), ("buy", 5), ("sell", 6)])
        self.click("nav_signal")
        self.click("nav_signal")
        self.click("nav_signal")
        figure = self.chart()
        names = [t["name"] for t in figure["data"]]
        self.assertIn("z-score", names)
        self.assertIn("Rolling mean (lookback 4)", names)
        levels = sorted(s["y0"] for s in figure["layout"]["shapes"] if s["type"] == "line")
        self.assertEqual(levels, [-1.5, -0.5, 0.5, 1.5])
        self.assertIn("entry_sell", page_text(self.at))

    def test_a_valid_run_with_no_signals_says_so_and_has_nothing_to_jump_to(self):
        self.choose("ui_builtin_key", "constant_price")
        self.click("ui_restore")
        self.run_app()
        self.assertEqual(len(self.displayed.document.signals), 0)
        self.assertIn("produced no signal requests", page_text(self.at))
        self.assertTrue(self.at.button(key="nav_signal").disabled)
        self.at.slider(key="lab_slider").set_value(30).run()
        self.assertIn("No signal requests in the visible prefix", page_text(self.at))

    def test_runner_warnings_are_shown(self):
        self.click("ui_restore")                      # 5/20 on a 9-bar fixture: cannot leave warm-up
        self.run_app()
        self.assertIn("fewer_bars_than_warmup", page_text(self.at))

    def test_two_symbols_filter_next_signal_and_counts(self):
        self.choose("ui_builtin_key", "two_symbols_interleaved")
        self.run_app()
        model = self.displayed.model
        self.assertEqual(model.symbols, ("AAPL", "MSFT"))
        everything = [s.event_index + 1 for s in model.signals]
        self.choose("lab_symbol", "MSFT")
        self.assertEqual(self.cursor, 0)                                          # filtering did not move the cursor
        self.click("nav_signal")
        self.assertEqual(self.cursor, model.next_signal(0, "MSFT"))
        self.assertEqual(model.events[self.cursor - 1].symbol, "MSFT")
        titles = [a["text"] for a in self.chart()["layout"]["annotations"] if ":" in a["text"]]
        self.assertTrue(all(t.startswith("MSFT") for t in titles), titles)
        self.choose("lab_symbol", model.all_label)
        self.assertEqual(self.cursor, model.next_signal(0, "MSFT"))
        self.assertGreater(len(everything), len([s for s in model.signals if s.symbol == "MSFT"]) - 1)
        self.assertEqual(len(self.launches), 1)

    def test_next_bar_with_a_filter_skips_the_other_symbols_rows(self):
        self.choose("ui_builtin_key", "two_symbols_interleaved")
        self.run_app()
        self.choose("lab_symbol", "MSFT")
        seen = []
        for _ in range(3):
            self.click("nav_next")
            seen.append(self.displayed.model.events[self.cursor - 1].symbol)
        self.assertEqual(seen, ["MSFT"] * 3)

    def test_equal_timestamp_fixture_keeps_file_order_in_the_replay(self):
        self.choose("ui_builtin_key", "tie_msft_first")
        self.run_app()
        events = self.displayed.document.events
        self.assertEqual([events[0].symbol, events[1].symbol], ["MSFT", "AAPL"])
        self.assertEqual(events[0].exchange_time, events[1].exchange_time)
        self.click("nav_next")
        self.assertEqual(self.displayed.model.selected_event(self.cursor).symbol, "MSFT")
        self.click("nav_next")
        self.assertEqual(self.displayed.model.selected_event(self.cursor).symbol, "AAPL")


class Uploads(AppTestCase):
    def upload(self, name, data):
        self.at.radio(key="ui_dataset_mode").set_value("Upload CSV").run()
        self.at.file_uploader(key="ui_upload").upload(name, data, "text/csv").run()
        self.assertFalse(self.at.exception, [e.value for e in self.at.exception])

    def test_run_is_disabled_until_a_file_is_chosen(self):
        self.at.radio(key="ui_dataset_mode").set_value("Upload CSV").run()
        self.assertTrue(self.at.button(key="ui_run").disabled)

    def test_a_valid_upload_runs_and_is_labelled_unknown_provenance(self):
        self.upload("my_data.csv", VALID)
        self.assertEqual(self.at.text_input(key="ui_symbols").value, "XYZ")
        self.set_number("sma_crossover", "short_window", 1)
        self.set_number("sma_crossover", "long_window", 3)
        self.run_app()
        run = self.displayed
        self.assertEqual((run.source.kind, run.source.display_name, run.source.synthetic), ("upload", "my_data.csv", False))
        self.assertEqual(run.source.sha256, datasets.sha256_hex(VALID))
        self.assertEqual(run.document.dataset.sha256, run.source.sha256)
        self.assertIn("provenance unknown", page_text(self.at).lower())
        self.assertEqual(len(self.launches), 1)

    def test_an_invalid_upload_shows_the_tools_line_and_column(self):
        bad = b"symbol,exchange_time,type,price\nXYZ,2026-03-02T14:30:00Z,bar,50\nXYZ,2026-03-02T14:31:00Z,bar,abc\n"
        self.upload("bad.csv", bad)
        self.run_app()
        failure = self.at.session_state["lab_failure"]
        self.assertEqual(failure.error.kind, "dataset_invalid")
        self.assertEqual((failure.error.problems[0].line, failure.error.problems[0].column), (3, "price"))
        self.assertIn("private copy of 'bad.csv'", page_text(self.at))
        self.assertNotIn("lab_displayed", self.at.session_state)                 # nothing to retain: no earlier success

    def test_an_empty_upload_is_refused_before_anything_starts(self):
        self.upload("empty.csv", b"")
        self.assertTrue(self.at.button(key="ui_run").disabled)
        self.assertIn("is empty", page_text(self.at))
        self.assertEqual(self.launches, [])

    def test_a_binary_upload_is_refused_before_anything_starts(self):
        self.upload("pic.csv", b"\x89PNG\r\n\x1a\n\x00\x00\x00")
        self.assertTrue(self.at.button(key="ui_run").disabled)
        self.assertIn("not a CSV text file", page_text(self.at))
        self.assertEqual(self.launches, [])

    def test_replacing_the_file_changes_the_snapshot_identity(self):
        self.upload("a.csv", VALID)
        self.set_number("sma_crossover", "short_window", 1)
        self.set_number("sma_crossover", "long_window", 3)
        self.run_app()
        self.at.file_uploader(key="ui_upload").upload("a.csv", VALID.replace(b",60", b",61")).run()
        self.assertIn("dataset", page_text(self.at))
        self.assertIn("These settings have changed", page_text(self.at))


class Setup(unittest.TestCase):
    def test_a_missing_executable_shows_what_to_do_and_nothing_else(self):
        error = LabUiError("executable_missing", "strategy_lab_replay has not been built.",
                           detail="Build with:\ncmake --build out/strategy_lab --config Release --target strategy_lab_replay")
        with mock.patch.object(bridge, "find_runner", side_effect=error):
            at = AppTest.from_file(APP, default_timeout=60).run()
        self.assertFalse(at.exception)
        text = page_text(at)
        self.assertIn("The replay executable was not found", text)
        self.assertIn("cmake --build out/strategy_lab", text)
        self.assertEqual(len(at.button), 1)                                       # only "Check again": no Run

    def test_an_executable_that_is_not_the_replay_tool_is_reported(self):
        from test_bridge import fake_runner
        with mock.patch.object(bridge, "find_runner", return_value=fake_runner()):
            at = AppTest.from_file(APP, default_timeout=60).run()
        self.assertFalse(at.exception)
        self.assertIn("could not be used", page_text(at))


if __name__ == "__main__":
    unittest.main()
