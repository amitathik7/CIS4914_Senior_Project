"""Figures: only the visible prefix is drawn, markers use shape and text, thresholds come from the run's configuration."""

import unittest

from support import ev, make_document, parse

from strategy_lab_ui import charts
from strategy_lab_ui.replay import ReplayModel

SMA_EVENTS = [
    ev("AAPL", 30, 3, verdict="warming_up", reason="warming_up", unavailable={"short_sma": "warming_up", "long_sma": "warming_up"}),
    ev("AAPL", 31, 2, indicators={"short_sma": 2.5, "long_sma": 2.0}),
    ev("AAPL", 32, 4, reason="crossover_buy", indicators={"short_sma": 3.0, "long_sma": 2.3}, signals=[(0, 1, "buy")]),
    ev("AAPL", 33, 1, reason="crossover_sell", indicators={"short_sma": 2.5, "long_sma": 2.6}, signals=[(0, 2, "sell")]),
]


def sma_model(events=SMA_EVENTS):
    doc = parse(make_document(events, parameters={"short_window": 2, "long_window": 3}))
    return ReplayModel(doc), doc.strategies[0]


def mr_model(events, **params):
    parameters = {"lookback": 4, "entry_threshold": 1.5, "rearm_threshold": 0.5, **params}
    doc = parse(make_document(events, kind="mean_reversion", parameters=parameters))
    return ReplayModel(doc), doc.strategies[0]


def trace(fig, name):
    return [t for t in fig.data if t.name == name]


class Prefix(unittest.TestCase):
    def test_position_zero_is_an_empty_state_with_no_data(self):
        model, config = sma_model()
        fig = charts.build_figure(model, config, 0, None, None)
        self.assertEqual(len(fig.data), 0)
        self.assertIn("nothing revealed", fig.layout.annotations[0].text)

    def test_only_revealed_rows_are_drawn(self):
        model, config = sma_model()
        for cursor in range(1, 5):
            fig = charts.build_figure(model, config, cursor, None, model.selected_event(cursor))
            revealed = {model.time_text(model.events[i]) for i in range(cursor)}
            for t in fig.data:
                self.assertTrue(set(t.x) <= revealed, (cursor, t.name))
            self.assertEqual(len(trace(fig, "Close")[0].x), cursor)

    def test_a_signal_marker_appears_only_with_its_event_and_goes_away_going_back(self):
        model, config = sma_model()
        at = lambda c: charts.build_figure(model, config, c, None, model.selected_event(c))
        self.assertEqual(trace(at(2), "Buy request"), [])
        self.assertEqual(len(trace(at(3), "Buy request")[0].x), 1)
        self.assertEqual(trace(at(3), "Sell request"), [])
        self.assertEqual(len(trace(at(4), "Sell request")[0].x), 1)
        self.assertEqual(trace(at(2), "Buy request"), [])                          # backwards removes it again

    def test_buy_and_sell_differ_by_shape_and_text_not_only_colour(self):
        model, config = sma_model()
        fig = charts.build_figure(model, config, 4, None, model.selected_event(4))
        buy, sell = trace(fig, "Buy request")[0], trace(fig, "Sell request")[0]
        self.assertEqual((buy.marker.symbol, sell.marker.symbol), ("triangle-up", "triangle-down"))
        self.assertEqual((list(buy.text), list(sell.text)), (["Buy"], ["Sell"]))
        self.assertIn("text", buy.mode)
        self.assertNotEqual(buy.marker.color, sell.marker.color)

    def test_markers_sit_at_the_signalling_events_close(self):
        model, config = sma_model()
        fig = charts.build_figure(model, config, 4, None, model.selected_event(4))
        self.assertEqual(list(trace(fig, "Buy request")[0].y), [4.0])
        self.assertEqual(list(trace(fig, "Sell request")[0].y), [1.0])

    def test_unavailable_indicator_values_are_gaps_not_zeros(self):
        model, config = sma_model()
        fig = charts.build_figure(model, config, 2, None, model.selected_event(2))
        self.assertEqual(list(trace(fig, "Short SMA (2)")[0].y), [None, 2.5])
        self.assertEqual(list(trace(fig, "Long SMA (3)")[0].y), [None, 2.0])

    def test_every_revealed_row_is_accounted_for_by_a_status_marker(self):
        model, config = sma_model()
        fig = charts.build_figure(model, config, 4, None, model.selected_event(4))
        self.assertEqual(len(trace(fig, "Warming up (no indicators yet)")[0].x), 1)
        self.assertEqual(len(trace(fig, "Evaluated bar")[0].x), 3)

    def test_the_selected_event_is_ringed(self):
        model, config = sma_model()
        fig = charts.build_figure(model, config, 2, None, model.selected_event(2))
        ring = trace(fig, "Selected event")[0]
        self.assertEqual(list(ring.x), [model.time_text(model.events[1])])

    def test_series_names_use_the_configured_windows(self):
        model, config = sma_model()
        fig = charts.build_figure(model, config, 3, None, model.selected_event(3))
        self.assertTrue(trace(fig, "Short SMA (2)") and trace(fig, "Long SMA (3)"))


class MeanReversion(unittest.TestCase):
    EVENTS = [ev("A", 30, 10, verdict="warming_up", reason="warming_up"),
              ev("A", 31, 6, reason="entry_buy", indicators={"mean": 9.0, "standard_deviation": 1.0, "z_score": -1.7},
                 signals=[(0, 1, "buy")]),
              ev("A", 32, 9, indicators={"mean": 9.0, "standard_deviation": 1.0, "z_score": 0.2})]

    def test_a_separate_z_panel_with_the_configured_thresholds(self):
        model, config = mr_model(self.EVENTS, entry_threshold=1.5, rearm_threshold=0.5)
        fig = charts.build_figure(model, config, 3, None, model.selected_event(3))
        levels = sorted(shape.y0 for shape in fig.layout.shapes if shape.type == "line")
        self.assertEqual(levels, [-1.5, -0.5, 0.5, 1.5])
        self.assertTrue(trace(fig, "z-score"))
        self.assertTrue(trace(fig, "Rolling mean (lookback 4)"))
        self.assertEqual(list(trace(fig, "z-score")[0].y), [None, -1.7, 0.2])

    def test_other_thresholds_move_the_lines(self):
        model, config = mr_model(self.EVENTS, entry_threshold=2.25, rearm_threshold=0.25)
        fig = charts.build_figure(model, config, 3, None, model.selected_event(3))
        self.assertEqual(sorted(s.y0 for s in fig.layout.shapes if s.type == "line"), [-2.25, -0.25, 0.25, 2.25])

    def test_the_signal_is_marked_on_both_panels(self):
        model, config = mr_model(self.EVENTS)
        fig = charts.build_figure(model, config, 3, None, model.selected_event(3))
        buys = trace(fig, "Buy request")
        self.assertEqual(len(buys), 2)
        self.assertEqual(sorted(list(b.y) for b in buys), [[-1.7], [6.0]])

    def test_the_price_panel_is_not_given_a_z_line(self):
        model, config = sma_model()
        fig = charts.build_figure(model, config, 3, None, model.selected_event(3))
        self.assertFalse(trace(fig, "z-score"))
        self.assertEqual(len([s for s in fig.layout.shapes]), 0)


class Symbols(unittest.TestCase):
    EVENTS = [ev("AAPL", 30, 10), ev("MSFT", 30, 20), ev("AAPL", 31, 11, signals=[(0, 1, "buy")]), ev("MSFT", 31, 21)]

    def test_all_symbols_stack_one_panel_per_symbol(self):
        model, config = sma_model(self.EVENTS)
        fig = charts.build_figure(model, config, 4, None, model.selected_event(4))
        titles = [a.text for a in fig.layout.annotations]
        self.assertEqual([t for t in titles if ":" in t], ["AAPL: close and indicators", "MSFT: close and indicators"])

    def test_the_filter_draws_one_symbol_and_hides_the_others_signals(self):
        model, config = sma_model(self.EVENTS)
        fig = charts.build_figure(model, config, 4, "MSFT", model.selected_event(4))
        self.assertEqual([a.text for a in fig.layout.annotations], ["MSFT: close and indicators"])
        self.assertEqual(trace(fig, "Buy request"), [])
        self.assertEqual(len(trace(fig, "Close")[0].x), 2)

    def test_more_than_four_symbols_are_capped_with_a_note(self):
        events = [ev(s, 30, 1) for s in "ABCDEF"]
        model, config = sma_model(events)
        fig = charts.build_figure(model, config, 6, None, model.selected_event(6))
        self.assertEqual(len([a for a in fig.layout.annotations if ":" in a.text]), charts.MAX_SYMBOL_PANELS)
        self.assertIn("first 4 of 6", charts.panel_note(model, None))
        self.assertIsNone(charts.panel_note(model, "A"))

    def test_a_symbol_with_nothing_revealed_yet_says_so(self):
        model, config = sma_model(self.EVENTS)
        fig = charts.build_figure(model, config, 1, "MSFT", model.selected_event(1))
        self.assertTrue(any("No MSFT rows revealed" in a.text for a in fig.layout.annotations))

    def test_trade_rows_are_drawn_apart_from_bars(self):
        events = [ev("A", 30, 10), ev("A", 30, 10.01, type="trade", verdict="ignored", reason="not_a_bar"), ev("A", 31, 11)]
        model, config = sma_model(events)
        fig = charts.build_figure(model, config, 3, None, model.selected_event(3))
        self.assertEqual(len(trace(fig, "Trade row (strategies ignore it)")[0].x), 1)
        self.assertEqual(len(trace(fig, "Close")[0].x), 2)               # the line joins bars only


if __name__ == "__main__":
    unittest.main()
