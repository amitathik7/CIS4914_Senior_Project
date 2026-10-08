"""Dataset sources: built-ins, uploads (sanity only; the replay tool validates), symbol suggestions and allowlist text."""

import unittest

from support import FIXTURES

from strategy_lab_ui import datasets
from strategy_lab_ui.errors import LabUiError


class Builtins(unittest.TestCase):
    def test_every_builtin_loads_and_is_marked_synthetic(self):
        keys = [d.key for d in datasets.BUILTINS]
        self.assertEqual(len(keys), len(set(keys)))
        for item in datasets.BUILTINS:
            source = datasets.load_builtin(item.key)
            self.assertTrue(source.synthetic)
            self.assertEqual(source.kind, "builtin")
            self.assertEqual(len(source.sha256), 64)
            self.assertEqual(source.sha256, datasets.sha256_hex((FIXTURES / item.filename).read_bytes()))
            self.assertIn("Synthetic", source.provenance)

    def test_presets_only_name_symbols_that_exist_in_the_file(self):
        for item in datasets.BUILTINS:
            present = set(datasets.load_builtin(item.key).symbols)
            for kind, preset in item.presets.items():
                self.assertTrue(set(preset["symbols"].split(",")) <= present, (item.key, kind))

    def test_a_missing_fixture_is_a_clear_error(self):
        with self.assertRaises(LabUiError) as caught:
            datasets.load_builtin("sma_crossover", directory=FIXTURES / "nowhere")
        self.assertEqual(caught.exception.kind, "input_missing")


class Uploads(unittest.TestCase):
    def refused(self, name, data, kind):
        with self.assertRaises(LabUiError) as caught:
            datasets.from_upload(name, data)
        self.assertEqual(caught.exception.kind, kind)
        self.assertIn(name or "upload.csv", caught.exception.message)

    def test_empty_file(self):
        self.refused("empty.csv", b"", "input_empty")

    def test_too_large(self):
        self.refused("big.csv", b"a" * (datasets.MAX_UPLOAD_BYTES + 1), "input_too_large")

    def test_exactly_the_limit_is_accepted(self):
        datasets.from_upload("ok.csv", b"a" * datasets.MAX_UPLOAD_BYTES)

    def test_binary_file(self):
        self.refused("x.csv", b"PK\x03\x04\x00\x00data", "input_not_text")

    def test_an_upload_is_unknown_provenance_never_synthetic(self):
        source = datasets.from_upload("mine.csv", b"symbol,exchange_time,type,price\nXYZ,2026-01-01T00:00:00Z,bar,5\n")
        self.assertFalse(source.synthetic)
        self.assertIn("unknown", source.provenance.lower())
        self.assertEqual((source.kind, source.display_name, source.symbols), ("upload", "mine.csv", ("XYZ",)))

    def test_upload_identity_is_its_content_hash(self):
        a = datasets.from_upload("a.csv", b"symbol,exchange_time,type,price\nA,2026-01-01T00:00:00Z,bar,5\n")
        b = datasets.from_upload("renamed.csv", b"symbol,exchange_time,type,price\nA,2026-01-01T00:00:00Z,bar,5\n")
        c = datasets.from_upload("a.csv", b"symbol,exchange_time,type,price\nA,2026-01-01T00:00:00Z,bar,6\n")
        self.assertEqual(a.sha256, b.sha256)
        self.assertNotEqual(a.sha256, c.sha256)

    def test_a_blank_name_still_gets_a_label(self):
        self.assertEqual(datasets.from_upload("", b"x").display_name, "upload.csv")


class SymbolSuggestion(unittest.TestCase):
    def test_distinct_symbols_in_first_seen_order(self):
        data = b"symbol,exchange_time,type,price\nMSFT,t,bar,1\nAAPL,t,bar,1\nMSFT,t,bar,1\n"
        self.assertEqual(datasets.detect_symbols(data), ("MSFT", "AAPL"))

    def test_bom_crlf_and_column_order(self):
        data = b"\xef\xbb\xbfprice,type,symbol\r\n1,bar,AAA\r\n2,bar,BBB\r\n"
        self.assertEqual(datasets.detect_symbols(data), ("AAA", "BBB"))

    def test_garbage_yields_no_suggestion_and_never_raises(self):
        for data in (b"", b"\xff\xfe\x00junk", b"no header here", b"a,b,c\n1,2,3\n", b"\n\n\n"):
            self.assertEqual(datasets.detect_symbols(data), ())

    def test_short_rows_are_skipped(self):
        self.assertEqual(datasets.detect_symbols(b"price,symbol\n1\n2,X\n"), ("X",))


class AllowlistText(unittest.TestCase):
    def test_spaces_around_commas_are_ignored_and_nothing_else_changes(self):
        self.assertEqual(datasets.symbols_param("AAPL, MSFT"), "AAPL,MSFT")
        self.assertEqual(datasets.symbols_param("  AAPL \t,\tMSFT  "), "AAPL,MSFT")

    def test_case_is_preserved(self):
        self.assertEqual(datasets.symbols_param("aapl,AaPl"), "aapl,AaPl")

    def test_empty_entries_are_left_for_the_replay_tool_to_reject(self):
        self.assertEqual(datasets.symbols_param("A,,B"), "A,,B")
        self.assertEqual(datasets.symbols_param(""), "")
        self.assertEqual(datasets.symbols_param("A,"), "A,")

    def test_non_ascii_text_is_not_damaged(self):
        self.assertEqual(datasets.symbols_param("ÄPPL, 日本"), "ÄPPL,日本")


if __name__ == "__main__":
    unittest.main()
