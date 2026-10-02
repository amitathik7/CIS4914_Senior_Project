"""Replay over a finished result: prefix selection, the cursor, display filters, event/signal association."""

import unittest

from support import ev, make_document, parse

from strategy_lab_ui.replay import ReplayModel, plotly_time


def two_symbols():
    """Interleaved AAPL/MSFT at equal timestamps; signals on events 1 (MSFT), 4 (AAPL) and 5 (MSFT)."""
    return parse(make_document([
        ev("AAPL", 30, 10), ev("MSFT", 30, 20, signals=[(0, 1, "buy")]),
        ev("AAPL", 31, 11), ev("MSFT", 31, 21),
        ev("AAPL", 32, 12, signals=[(0, 2, "sell")]), ev("MSFT", 32, 22, signals=[(0, 3, "sell")]),
        ev("AAPL", 33, 13)]))


class Prefix(unittest.TestCase):
    def setUp(self):
        self.model = ReplayModel(two_symbols())

    def test_position_zero_shows_nothing(self):
        self.assertEqual(self.model.events_through(0), [])
        self.assertEqual(self.model.signals_through(0), [])
        self.assertIsNone(self.model.selected_event(0))
        counts = self.model.counts(0)
        self.assertEqual((counts.events, counts.signals), (0, 0))
        self.assertEqual(sum(counts.verdicts.values()), 0)

    def test_the_prefix_is_exactly_the_first_k_events_in_recorded_order(self):
        for k in range(self.model.total + 1):
            self.assertEqual([e.index for e in self.model.events_through(k)], list(range(k)))

    def test_the_selected_event_is_the_last_revealed(self):
        self.assertEqual(self.model.selected_event(1).index, 0)
        self.assertEqual(self.model.selected_event(7).index, 6)

    def test_signals_appear_only_once_their_event_is_revealed(self):
        self.assertEqual([s.signal_id for s in self.model.signals_through(1)], [])
        self.assertEqual([s.signal_id for s in self.model.signals_through(2)], [1])
        self.assertEqual([s.signal_id for s in self.model.signals_through(5)], [1, 2])
        self.assertEqual([s.signal_id for s in self.model.signals_through(7)], [1, 2, 3])

    def test_going_back_removes_what_was_in_the_future(self):
        forward = [self.model.counts(k) for k in range(8)]
        backward = [self.model.counts(k) for k in reversed(range(8))][::-1]
        self.assertEqual(forward, backward)
        self.assertEqual(self.model.counts(7).signals, 3)
        self.assertEqual(self.model.counts(3).signals, 1)
        self.assertEqual(self.model.counts(1).signals, 0)

    def test_the_symbol_filter_only_hides_rows(self):
        self.assertEqual([e.symbol for e in self.model.events_through(6, "MSFT")], ["MSFT"] * 3)
        self.assertEqual([s.symbol for s in self.model.signals_through(7, "AAPL")], ["AAPL"])
        counts = self.model.counts(7, "MSFT")
        self.assertEqual((counts.events, counts.buys, counts.sells), (3, 1, 1))

    def test_counts_cover_every_revealed_event_exactly_once(self):
        counts = self.model.counts(7)
        self.assertEqual(sum(counts.verdicts.values()), counts.events)
        self.assertEqual(sum(counts.reasons.values()), counts.events)


class EqualTimestamps(unittest.TestCase):
    def test_rows_with_the_same_timestamp_stay_distinct_and_keep_their_own_signals(self):
        model = ReplayModel(parse(make_document([
            ev("MSFT", 30, signals=[(0, 1, "buy")]), ev("AAPL", 30, signals=[(0, 2, "sell")])])))
        self.assertEqual(model.events[0].exchange_time, model.events[1].exchange_time)
        self.assertEqual([s.signal_id for s in model.signals_on(0)], [1])
        self.assertEqual([s.signal_id for s in model.signals_on(1)], [2])
        self.assertEqual([s.symbol for s in model.signals_through(1)], ["MSFT"])     # only the first row is revealed
        self.assertEqual([s.symbol for s in model.signals_through(2)], ["MSFT", "AAPL"])

    def test_signals_are_joined_by_event_index_not_by_time(self):
        model = ReplayModel(parse(make_document([
            ev("MSFT", 30, time="2026-01-05T14:30:00.000000000Z"),
            ev("AAPL", 30, time="2026-01-05T14:30:00.000000000Z", signals=[(0, 7, "buy")])])))
        self.assertEqual(model.signals_on(0), ())
        self.assertEqual(model.signals_on(1)[0].symbol, "AAPL")
        self.assertEqual(model.counts(1).signals, 0)
        self.assertEqual(model.counts(2).signals, 1)


class Navigation(unittest.TestCase):
    def setUp(self):
        self.model = ReplayModel(two_symbols())

    def test_next_bar_and_previous_walk_the_recorded_order(self):
        self.assertEqual([self.model.next_bar(c) for c in (0, 3, 6)], [1, 4, 7])
        self.assertIsNone(self.model.next_bar(7))
        self.assertEqual([self.model.previous_bar(c) for c in (1, 4, 7)], [0, 3, 6])
        self.assertIsNone(self.model.previous_bar(0))

    def test_next_bar_with_a_filter_steps_over_other_symbols(self):
        # MSFT rows are events 1, 3, 5 -> positions 2, 4, 6
        self.assertEqual(self.model.next_bar(0, "MSFT"), 2)
        self.assertEqual(self.model.next_bar(2, "MSFT"), 4)
        self.assertEqual(self.model.next_bar(4, "MSFT"), 6)
        self.assertIsNone(self.model.next_bar(6, "MSFT"))
        self.assertEqual(self.model.next_bar(3, "MSFT"), 4)        # from a position showing an AAPL row

    def test_previous_with_a_filter_ends_at_zero(self):
        self.assertEqual(self.model.previous_bar(6, "MSFT"), 4)
        self.assertEqual(self.model.previous_bar(4, "MSFT"), 2)
        self.assertEqual(self.model.previous_bar(2, "MSFT"), 0)
        self.assertEqual(self.model.previous_bar(3, "MSFT"), 2)
        self.assertIsNone(self.model.previous_bar(0, "MSFT"))

    def test_next_signal_without_a_filter_visits_every_signalling_event(self):
        positions, cursor = [], 0
        while (cursor := self.model.next_signal(cursor)) is not None:
            positions.append(cursor)
        self.assertEqual(positions, [2, 5, 6])

    def test_next_signal_respects_the_display_filter(self):
        self.assertEqual(self.model.next_signal(0, "AAPL"), 5)
        self.assertIsNone(self.model.next_signal(5, "AAPL"))
        self.assertEqual(self.model.next_signal(0, "MSFT"), 2)
        self.assertEqual(self.model.next_signal(2, "MSFT"), 6)
        self.assertIsNone(self.model.next_signal(6, "MSFT"))

    def test_next_signal_from_inside_the_run_never_goes_backwards(self):
        for cursor in range(self.model.total + 1):
            target = self.model.next_signal(cursor)
            self.assertTrue(target is None or target > cursor)

    def test_a_run_with_no_signals_has_no_next_signal(self):
        model = ReplayModel(parse(make_document([ev("A", 30), ev("A", 31)])))
        self.assertIsNone(model.next_signal(0))
        self.assertEqual(model.counts(2).signals, 0)

    def test_two_strategies_on_one_event_are_one_stop_and_neither_is_lost(self):
        document = parse(make_document([ev("A", 30), ev("A", 31, signals=[(0, 1, "buy"), (1, 1, "sell")]), ev("A", 32)],
                                       strategies=("s1", "s2")))
        first, second = ReplayModel(document, "s1"), ReplayModel(document, "s2")
        self.assertEqual(first.next_signal(0), 2)
        self.assertEqual(second.next_signal(0), 2)
        self.assertIsNone(first.next_signal(2))
        self.assertEqual([(s.strategy_id, s.signal_id) for s in document.signals_by_event[1]], [("s1", 1), ("s2", 1)])
        self.assertEqual([s.side for s in first.signals_on(1)], ["buy"])
        self.assertEqual([s.side for s in second.signals_on(1)], ["sell"])

    def test_a_strategy_never_sees_another_strategys_signals(self):
        document = parse(make_document([ev("A", 30, signals=[(0, 1, "buy")]), ev("A", 31),
                                        ev("A", 32, signals=[(1, 1, "sell")])], strategies=("s1", "s2")))
        first, second = ReplayModel(document, "s1"), ReplayModel(document, "s2")
        self.assertEqual([first.next_signal(0), first.next_signal(1)], [1, None])
        self.assertEqual([second.next_signal(0), second.next_signal(3)], [3, None])
        self.assertEqual((first.counts(3).buys, first.counts(3).sells), (1, 0))
        self.assertEqual((second.counts(3).buys, second.counts(3).sells), (0, 1))
        self.assertEqual([s.strategy_id for s in first.signals_through(3)], ["s1"])

    def test_several_signals_of_one_strategy_on_one_event_are_all_kept(self):
        document = parse(make_document([ev("A", 30, signals=[(0, 1, "buy"), (0, 2, "sell")])]))
        model = ReplayModel(document)
        self.assertEqual([s.signal_id for s in model.signals_on(0)], [1, 2])
        self.assertEqual(model.counts(1).signals, 2)
        self.assertEqual(model.next_signal(0), 1)

    def test_clamp(self):
        self.assertEqual([self.model.clamp(c) for c in (-4, 0, 3, 7, 99)], [0, 0, 3, 7, 7])


class Series(unittest.TestCase):
    def test_a_series_holds_only_the_revealed_rows_of_one_symbol(self):
        model = ReplayModel(parse(make_document([
            ev("AAPL", 30, 10, verdict="warming_up", reason="warming_up", unavailable={"short_sma": "warming_up"}),
            ev("MSFT", 30, 20),
            ev("AAPL", 31, 11, indicators={"short_sma": 10.5}),
            ev("AAPL", 32, 12, indicators={"short_sma": 11.5})])))
        series = model.series("AAPL", 3, ("short_sma",))
        self.assertEqual(series.event_indexes, [0, 2])
        self.assertEqual(series.prices, [10.0, 11.0])
        self.assertEqual(series.indicators["short_sma"], [None, 10.5])           # an unavailable value is a gap
        self.assertEqual(series.verdicts, ["warming_up", "evaluated"])
        self.assertEqual(model.series("AAPL", 0, ("short_sma",)).event_indexes, [])

    def test_plotly_time_keeps_millisecond_resolution_only_for_placement(self):
        self.assertEqual(plotly_time("2026-01-05T14:30:00.123456789Z"), "2026-01-05 14:30:00.123")
        self.assertEqual(plotly_time("2026-01-05T14:30:00Z"), "2026-01-05 14:30:00")
        self.assertEqual(plotly_time("2026-01-05T14:30:00.5Z"), "2026-01-05 14:30:00.500")

    def test_the_all_symbols_label_cannot_collide_with_a_real_symbol(self):
        model = ReplayModel(parse(make_document([ev("All symbols", 30), ev("AAPL", 30)])))
        self.assertNotIn(model.all_label, model.symbols)
        self.assertEqual(model.all_label, "All symbols*")
        self.assertEqual(ReplayModel(two_symbols()).all_label, "All symbols")


if __name__ == "__main__":
    unittest.main()
