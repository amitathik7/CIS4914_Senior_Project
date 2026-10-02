"""Switching between Explore, Compare and Validate: each view keeps its own settings, results, position and filters, views do not
touch each other's state, and switching never starts the engine. AppTest against the real executable.

Streamlit forgets the value of a widget that was not drawn in a run; persist.py is what keeps the settings, and these tests are
what proves it (a plain widget would come back empty, as the first test shows).
"""

import unittest

from support import APP, ProcessCounter, VALID_CSV, open_view, page_text, real_runner

from streamlit.testing.v1 import AppTest


class NavigationCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = real_runner()

    def setUp(self):
        self.counter = ProcessCounter()
        self.counter.__enter__()
        self.addCleanup(self.counter.__exit__)
        self.at = AppTest.from_file(APP, default_timeout=120).run()

    def click(self, key):
        self.at.button(key=key).click().run()
        self.assertFalse(self.at.exception, [e.value for e in self.at.exception])


class TheSwitch(NavigationCase):
    def test_explore_is_the_default_and_the_three_views_are_offered(self):
        self.assertEqual(self.at.radio(key="ui_view").value, "Explore")
        self.assertEqual(list(self.at.radio(key="ui_view").options), ["Explore", "Compare", "Validate"])
        self.assertIn("press **Run replay**", page_text(self.at))

    def test_each_view_shows_its_own_content(self):
        open_view(self.at, "Compare")
        self.assertIn("press **Run comparison**", page_text(self.at))
        self.assertNotIn("press **Run replay**", page_text(self.at))
        open_view(self.at, "Validate")
        self.assertIn("Nothing has been run in this session", page_text(self.at))
        open_view(self.at, "Explore")
        self.assertIn("press **Run replay**", page_text(self.at))

    def test_switching_views_never_starts_the_engine(self):
        for view in ("Compare", "Validate", "Explore", "Validate", "Compare", "Explore"):
            open_view(self.at, view)
        self.assertEqual(self.counter.runs, 0)
        # The catalog is cached per executable build for the whole server process: at most one `describe` (none if an earlier
        # test in this process already read it), never one per view or per switch.
        self.assertLessEqual(self.counter.calls.count("describe"), 1)

    def test_a_plain_streamlit_widget_would_lose_its_value_but_the_lab_s_widgets_do_not(self):
        """The reason persist.py exists: without it the edited value is gone after a visit to another view."""
        self.at.number_input(key="ui_param.sma_crossover.short_window").set_value(1).run()
        open_view(self.at, "Compare")
        self.assertNotIn("ui_param.sma_crossover.short_window", self.at.session_state)       # Streamlit dropped it ...
        open_view(self.at, "Explore")
        self.assertEqual(self.at.number_input(key="ui_param.sma_crossover.short_window").value, 1)    # ... and persist restored it


class ExploreKeepsItsState(NavigationCase):
    def test_settings_result_position_and_filter_survive_a_visit_to_compare(self):
        self.click("ui_run")
        self.click("nav_signal")
        self.at.number_input(key="ui_param.sma_crossover.short_window").set_value(1).run()
        shown, cursor = self.at.session_state["lab_displayed"], self.at.session_state["lab_cursor"]
        self.assertEqual(cursor, 5)

        open_view(self.at, "Compare")
        open_view(self.at, "Validate")
        open_view(self.at, "Explore")

        self.assertIs(self.at.session_state["lab_displayed"], shown)
        self.assertEqual(self.at.session_state["lab_cursor"], 5)
        self.assertEqual(self.at.slider(key="lab_slider").value, 5)
        self.assertEqual(self.at.number_input(key="ui_param.sma_crossover.short_window").value, 1)
        self.assertEqual(self.at.selectbox(key="ui_strategy_kind").value, "sma_crossover")
        self.assertEqual(self.at.text_input(key="ui_symbols").value, "AAPL")
        text = page_text(self.at)
        self.assertIn("Event 5 of 9", text)
        self.assertIn("These settings have changed since the displayed run", text)       # the pending edit is still pending
        self.assertEqual(self.counter.runs, 1)

    def test_the_dataset_and_strategy_choices_survive(self):
        self.at.selectbox(key="ui_builtin_key").set_value("mr_rearm").run()
        self.at.selectbox(key="ui_strategy_kind").set_value("mean_reversion").run()
        open_view(self.at, "Compare")
        open_view(self.at, "Explore")
        self.assertEqual(self.at.selectbox(key="ui_builtin_key").value, "mr_rearm")
        self.assertEqual(self.at.selectbox(key="ui_strategy_kind").value, "mean_reversion")
        self.assertEqual(self.at.number_input(key="ui_param.mean_reversion.lookback").value, 4)

    def test_a_failed_run_and_the_retained_result_survive(self):
        self.click("ui_run")
        self.at.number_input(key="ui_param.sma_crossover.long_window").set_value(1).run()
        self.at.number_input(key="ui_param.sma_crossover.short_window").set_value(1).run()
        self.click("ui_run")
        self.assertIsNotNone(self.at.session_state["lab_failure"])
        open_view(self.at, "Validate")
        open_view(self.at, "Explore")
        self.assertIsNotNone(self.at.session_state["lab_failure"])
        self.assertIn("last successful run", page_text(self.at))


class CompareKeepsItsState(NavigationCase):
    def test_settings_result_position_and_filter_survive_a_visit_to_explore(self):
        open_view(self.at, "Compare")
        self.click("cmp_quick_two_sma")
        self.click("cmp_run")
        self.click("cmp_nav_signal")
        self.click("cmp_nav_signal")
        self.at.number_input(key="cmp_b_param.sma_crossover.long_window").set_value(4).run()
        shown = self.at.session_state["cmp_displayed"]
        self.assertEqual(self.at.session_state["cmp_cursor"], 5)

        open_view(self.at, "Explore")
        self.click("ui_run")                                   # Explore does its own work meanwhile
        open_view(self.at, "Compare")

        self.assertIs(self.at.session_state["cmp_displayed"], shown)
        self.assertEqual(self.at.session_state["cmp_cursor"], 5)
        self.assertEqual(self.at.slider(key="cmp_slider").value, 5)
        self.assertEqual(self.at.number_input(key="cmp_b_param.sma_crossover.long_window").value, 4)
        self.assertEqual(self.at.selectbox(key="cmp_b_kind").value, "sma_crossover")
        text = page_text(self.at)
        self.assertIn("These settings have changed since the displayed comparison", text)
        self.assertIn("B: parameters (long_window)", text)
        self.assertEqual(self.counter.runs, 2 + 1, "two for the comparison, one for Explore; the switches added none")

    def test_the_display_filter_survives(self):
        open_view(self.at, "Compare")
        self.at.selectbox(key="cmp_builtin_key").set_value("two_symbols_interleaved").run()
        self.click("cmp_run")
        self.at.selectbox(key="cmp_symbol").set_value("MSFT").run()
        open_view(self.at, "Validate")
        open_view(self.at, "Compare")
        self.assertEqual(self.at.selectbox(key="cmp_symbol").value, "MSFT")


class ViewsAreIndependent(NavigationCase):
    def test_choosing_a_dataset_in_compare_does_not_change_explore_and_vice_versa(self):
        open_view(self.at, "Compare")
        self.at.selectbox(key="cmp_builtin_key").set_value("mr_rearm").run()
        self.at.selectbox(key="cmp_a_kind").set_value("mean_reversion").run()
        open_view(self.at, "Explore")
        self.assertEqual(self.at.selectbox(key="ui_builtin_key").value, "sma_crossover")
        self.assertEqual(self.at.selectbox(key="ui_strategy_kind").value, "sma_crossover")
        self.at.selectbox(key="ui_builtin_key").set_value("constant_price").run()
        open_view(self.at, "Compare")
        self.assertEqual(self.at.selectbox(key="cmp_builtin_key").value, "mr_rearm")
        self.assertEqual(self.at.selectbox(key="cmp_a_kind").value, "mean_reversion")

    def test_a_run_in_one_view_does_not_appear_in_another(self):
        self.click("ui_run")
        open_view(self.at, "Compare")
        self.assertNotIn("cmp_displayed", self.at.session_state)
        self.assertIn("press **Run comparison**", page_text(self.at))
        open_view(self.at, "Validate")
        self.assertNotIn("val_displayed", self.at.session_state)


class UploadsSurviveAVisitElsewhere(NavigationCase):
    """A file-uploader widget cannot be restored by Streamlit, so the bytes are kept in session state instead."""

    def test_an_uploaded_file_in_explore_is_still_in_use_after_a_visit_to_compare(self):
        self.at.radio(key="ui_dataset_mode").set_value("Upload CSV").run()
        self.at.file_uploader(key="ui_upload").upload("mine.csv", VALID_CSV, "text/csv").run()
        self.assertEqual(self.at.text_input(key="ui_symbols").value, "XYZ")
        open_view(self.at, "Compare")
        open_view(self.at, "Explore")
        self.assertEqual(self.at.radio(key="ui_dataset_mode").value, "Upload CSV")
        self.assertFalse(self.at.button(key="ui_run").disabled, "the kept file means Run is still possible")
        self.assertIn("Using the file uploaded earlier in this session: **mine.csv**", page_text(self.at))
        self.at.number_input(key="ui_param.sma_crossover.short_window").set_value(1).run()
        self.at.number_input(key="ui_param.sma_crossover.long_window").set_value(3).run()
        self.click("ui_run")
        run = self.at.session_state["lab_displayed"]
        self.assertEqual((run.source.display_name, run.source.kind), ("mine.csv", "upload"))

    def test_the_kept_file_can_be_forgotten(self):
        self.at.radio(key="ui_dataset_mode").set_value("Upload CSV").run()
        self.at.file_uploader(key="ui_upload").upload("mine.csv", VALID_CSV, "text/csv").run()
        open_view(self.at, "Validate")
        open_view(self.at, "Explore")
        self.click("ui_upload_forget")
        self.assertTrue(self.at.button(key="ui_run").disabled)
        self.assertIn("Choose a file to enable Run", page_text(self.at))

    def test_removing_the_file_with_the_uploaders_own_button_really_drops_it(self):
        """Regression (found in the browser): the uploader's remove button used to leave the kept copy in use."""
        self.at.radio(key="ui_dataset_mode").set_value("Upload CSV").run()
        self.at.file_uploader(key="ui_upload").upload("mine.csv", VALID_CSV, "text/csv").run()
        self.assertFalse(self.at.button(key="ui_run").disabled)
        self.at.file_uploader(key="ui_upload").set_value(None).run()
        self.assertIsNone(self.at.session_state["ui_upload_kept"])
        self.assertTrue(self.at.button(key="ui_run").disabled)
        text = page_text(self.at)
        self.assertIn("Choose a file to enable Run", text)
        self.assertNotIn("Using the file uploaded earlier", text)

    def test_arriving_from_another_view_does_not_count_as_removing_the_file(self):
        self.at.radio(key="ui_dataset_mode").set_value("Upload CSV").run()
        self.at.file_uploader(key="ui_upload").upload("mine.csv", VALID_CSV, "text/csv").run()
        for view in ("Validate", "Explore", "Compare", "Explore"):
            open_view(self.at, view)
        self.at.run()                                          # a further plain rerun in Explore must not forget it either
        self.assertIsNotNone(self.at.session_state["ui_upload_kept"])
        self.assertFalse(self.at.button(key="ui_run").disabled)

    def test_switching_the_source_to_the_built_in_data_and_back_keeps_the_upload(self):
        self.at.radio(key="ui_dataset_mode").set_value("Upload CSV").run()
        self.at.file_uploader(key="ui_upload").upload("mine.csv", VALID_CSV, "text/csv").run()
        self.at.radio(key="ui_dataset_mode").set_value("Built-in demo").run()
        self.at.radio(key="ui_dataset_mode").set_value("Upload CSV").run()
        self.assertEqual(self.at.session_state["ui_upload_kept"][0], "mine.csv")
        self.assertIn("Using the file uploaded earlier in this session: **mine.csv**", page_text(self.at))

    def test_compare_can_also_switch_between_its_upload_and_the_built_in_data_and_back(self):
        open_view(self.at, "Compare")
        self.at.radio(key="cmp_dataset_mode").set_value("Upload CSV").run()
        self.at.file_uploader(key="cmp_upload").upload("cmp.csv", VALID_CSV, "text/csv").run()
        self.at.radio(key="cmp_dataset_mode").set_value("Built-in demo").run()
        self.assertFalse(self.at.exception, [e.value for e in self.at.exception])
        self.assertEqual(self.at.selectbox(key="cmp_builtin_key").value, "sma_crossover")
        self.at.radio(key="cmp_dataset_mode").set_value("Upload CSV").run()
        self.assertEqual(self.at.session_state["cmp_upload_kept"][0], "cmp.csv")

    def test_each_view_keeps_its_own_upload(self):
        open_view(self.at, "Compare")
        self.at.radio(key="cmp_dataset_mode").set_value("Upload CSV").run()
        self.at.file_uploader(key="cmp_upload").upload("cmp.csv", VALID_CSV, "text/csv").run()
        open_view(self.at, "Explore")
        self.assertEqual(self.at.radio(key="ui_dataset_mode").value, "Built-in demo")
        open_view(self.at, "Compare")
        self.assertIn("Using the file uploaded earlier in this session: **cmp.csv**", page_text(self.at))


class ValidateKeepsItsResults(NavigationCase):
    def test_results_survive_and_nothing_reruns(self):
        open_view(self.at, "Validate")
        self.click("val_run")
        results = self.at.session_state["val_displayed"]
        runs = self.counter.runs
        open_view(self.at, "Explore")
        open_view(self.at, "Validate")
        self.assertIs(self.at.session_state["val_displayed"], results)
        self.assertEqual(self.counter.runs, runs)


if __name__ == "__main__":
    unittest.main()
