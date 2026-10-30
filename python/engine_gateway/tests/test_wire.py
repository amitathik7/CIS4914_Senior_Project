"""The wire format as the gateway reads it: topics, envelopes, exact decimals with the engine's rounding."""

import unittest

import support  # noqa: F401  (path setup)

from engine_gateway import wire


class Topics(unittest.TestCase):
    def test_prefix_and_type(self):
        self.assertEqual(wire.split_topic("te.demo.signal"), ("te.demo", "signal"))
        self.assertEqual(wire.split_topic("trading_engine.market_data"), ("trading_engine", "market_data"))
        self.assertIsNone(wire.split_topic("te.demo.portfolio"))
        self.assertIsNone(wire.split_topic("signal"))


class Envelopes(unittest.TestCase):
    def test_decode(self):
        e = wire.decode("te.signal", support.signal(3, "AAPL", "buy", 31))
        self.assertEqual((e.run_id, e.type, e.payload["id"]), ("te", "signal", 3))

    def test_refusals(self):
        good = support.signal(3, "AAPL", "buy", 31)
        for topic, value in [("te.order", good), ("te.signal", b"[1]"), ("te.signal", b"{"),
                             ("te.signal", good.replace(b'"schema": 1', b'"schema": 2'))]:
            with self.subTest(topic=topic, value=value[:20]), self.assertRaises(wire.WireError):
                wire.decode(topic, value)

    def test_unknown_members_are_ignored(self):
        value = support.signal(3, "AAPL", "buy", 31).replace(b'{"schema"', b'{"later": true, "schema"')
        self.assertEqual(wire.decode("te.signal", value).payload["symbol"], "AAPL")


class Decimals(unittest.TestCase):
    def test_round_trip(self):
        for text in ["0", "1", "150.02", "-0.01", "0.000001", "123456789.123456"]:
            self.assertEqual(wire.text(wire.micros(text)), text)
        self.assertEqual(wire.text(wire.micros("192.50")), "192.5")

    def test_not_decimal_text(self):
        for value in [1.5, "1e3", "1.1234567", "", " 1", "+1", None]:
            with self.subTest(value=value), self.assertRaises(wire.WireError):
                wire.micros(value)

    def test_divide_rounds_half_away_from_zero(self):
        self.assertEqual(wire.divide(5, 2), 3)
        self.assertEqual(wire.divide(-5, 2), -3)
        self.assertEqual(wire.divide(4, 3), 1)
        self.assertEqual(wire.divide(-4, 3), -1)
        self.assertEqual(wire.divide(7, -2), -4)
        self.assertEqual(wire.divide(0, 3), 0)


if __name__ == "__main__":
    unittest.main()
