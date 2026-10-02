"""The dark theme: one palette, three consumers (Streamlit's config file, the custom CSS, the Plotly charts), kept in step and readable.

These are checks of the files and figures. How the page really looks (computed styles of buttons, disabled controls, the browser's
colour-scheme preference) was checked in a real browser; see docs/STRATEGY_LAB.md.
"""

import re
import unittest
from pathlib import Path
from unittest import mock

from support import ROOT, make_document, ev, parse

from streamlit.testing.v1 import AppTest

from strategy_lab_ui import charts, theme
from strategy_lab_ui.replay import ReplayModel

CONFIG = ROOT / ".streamlit" / "config.toml"


def config_section(name: str) -> dict[str, str]:
    """The `key = "value"` lines of one [section] of the config file (no TOML library needed on Python 3.10)."""
    out: dict[str, str] = {}
    current = ""
    for line in CONFIG.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line.startswith("[") and line.endswith("]"):
            current = line[1:-1]
        elif current == name and "=" in line and not line.startswith("#"):
            key, _, value = line.partition("=")
            match = re.match(r'\s*"([^"]*)"|\s*([^\s#]+)', value)         # a quoted string, or a bare word/number
            out[key.strip()] = (match.group(1) if match.group(1) is not None else match.group(2)) if match else ""
    return out


class ConfigFile(unittest.TestCase):
    def test_the_theme_is_dark_and_pinned_by_the_app_scoped_config(self):
        theme_section = config_section("theme")
        self.assertEqual(theme_section["base"], "dark")
        self.assertEqual(theme_section["backgroundColor"].upper(), theme.BACKGROUND)
        self.assertEqual(theme_section["secondaryBackgroundColor"].upper(), theme.SURFACE)
        self.assertEqual(theme_section["textColor"].upper(), theme.TEXT)
        self.assertEqual(theme_section["primaryColor"].upper(), theme.ACCENT)
        self.assertEqual(theme_section["borderColor"].upper(), theme.BORDER)
        self.assertEqual(config_section("theme.sidebar")["backgroundColor"].upper(), theme.SURFACE)

    def test_the_status_colours_are_the_palette_ones(self):
        section = config_section("theme")
        self.assertEqual(section["greenColor"].upper(), theme.BUY)
        self.assertEqual(section["redColor"].upper(), theme.SELL)
        self.assertEqual(section["orangeColor"].upper(), theme.WARN)

    def test_runtime_settings_survived_the_theme_change(self):
        server = config_section("server")
        self.assertEqual(server["address"], "127.0.0.1")
        self.assertEqual(server["port"], "8501")
        self.assertEqual(server["maxUploadSize"], "8")
        self.assertEqual(server["enableXsrfProtection"], "true")
        self.assertEqual(config_section("browser")["gatherUsageStats"], "false")
        self.assertEqual(config_section("browser")["serverAddress"], "127.0.0.1")
        self.assertEqual(config_section("client")["toolbarMode"], "minimal")

    def test_the_launcher_starts_streamlit_from_the_directory_that_holds_the_config(self):
        script = (ROOT / "run_lab.ps1").read_text(encoding="utf-8")
        self.assertIn("Set-Location $here", script)
        self.assertLess(script.index("Set-Location $here"), script.index("-m streamlit run app.py"))
        self.assertIn("--server.address 127.0.0.1", script)
        self.assertTrue(CONFIG.is_file())

    def test_the_launcher_checks_and_names_the_config_it_relies_on(self):
        script = (ROOT / "run_lab.ps1").read_text(encoding="utf-8")
        self.assertIn('Join-Path $here ".streamlit\\config.toml"', script)
        self.assertIn("CANNOT START", script)
        self.assertIn('Write-Host "Config:       $config"', script)
        self.assertIn("STREAMLIT_THEME", script)                 # a theme override in the environment is announced
        self.assertTrue(script.isascii(), "the launcher must stay ASCII for Windows PowerShell 5.1")


class Readable(unittest.TestCase):
    TEXTS = {"primary text": theme.TEXT, "secondary text": theme.TEXT_MUTED, "accent": theme.ACCENT, "buy": theme.BUY,
             "sell": theme.SELL, "warning": theme.WARN}

    def test_every_text_colour_is_readable_on_the_page_and_on_the_cards(self):
        for name, colour in self.TEXTS.items():
            for surface_name, surface in (("page", theme.BACKGROUND), ("card", theme.SURFACE), ("raised", theme.SURFACE_RAISED)):
                self.assertGreaterEqual(theme.contrast_ratio(colour, surface), 4.5, f"{name} on {surface_name}")

    def test_the_filled_primary_button_text_is_readable(self):
        self.assertGreaterEqual(theme.contrast_ratio(theme.BACKGROUND, theme.ACCENT), 4.5)
        # What the page used to do: white on the accent. It is not readable, which is why the CSS overrides it.
        self.assertLess(theme.contrast_ratio("#FFFFFF", theme.ACCENT), 4.5)
        self.assertIn('button[kind="primary"]', theme.CSS)

    def test_chart_lines_stand_out_from_the_plotting_area_and_from_each_other(self):
        lines = {"price": theme.PRICE, "short": theme.SHORT_SMA, "long": theme.LONG_SMA, "mean": theme.MEAN, "z": theme.ZSCORE,
                 "buy": theme.BUY, "sell": theme.SELL, "warn": theme.WARN}
        for name, colour in lines.items():
            self.assertGreaterEqual(theme.contrast_ratio(colour, theme.SURFACE), 3.0, name)
        self.assertEqual(len({theme.SHORT_SMA, theme.LONG_SMA, theme.MEAN, theme.PRICE}), 4)
        self.assertGreaterEqual(theme.contrast_ratio(theme.MEMBER_A, theme.SURFACE), 3.0)
        self.assertGreaterEqual(theme.contrast_ratio(theme.MEMBER_B, theme.SURFACE), 3.0)
        self.assertNotEqual(theme.MEMBER_A, theme.MEMBER_B)

    def test_the_contrast_function_matches_known_values(self):
        self.assertAlmostEqual(theme.contrast_ratio("#000000", "#FFFFFF"), 21.0, places=6)
        self.assertAlmostEqual(theme.contrast_ratio("#FFFFFF", "#FFFFFF"), 1.0, places=6)


class NoLightLeftovers(unittest.TestCase):
    LIGHT = ("#F1F3F5", "#D5DAE0", "#E5E7EB", "#FFFFFF", "#F8F9FA", "plotly_white")

    def test_the_css_and_the_chart_module_hardcode_no_light_background(self):
        sources = {"theme.css": theme.CSS, "charts.py": (ROOT / "strategy_lab_ui" / "charts.py").read_text(encoding="utf-8"),
                   "views.py": (ROOT / "strategy_lab_ui" / "views.py").read_text(encoding="utf-8")}
        for name, text in sources.items():
            for light in self.LIGHT:
                self.assertNotIn(light.lower(), text.lower(), f"{name} still mentions {light}")

    def test_every_hex_colour_in_the_css_is_dark_or_a_named_accent(self):
        allowed_light = {theme.ACCENT, theme.MEMBER_A, theme.MEMBER_B}
        for colour in re.findall(r"#[0-9A-Fa-f]{6}", theme.CSS):
            self.assertTrue(theme.luminance(colour) < 0.2 or colour.upper() in allowed_light, colour)

    def test_the_sticky_action_bar_uses_the_card_colour_not_a_light_one(self):
        rule = re.search(r"run_bar\)[^}]*\}", theme.CSS, re.S)
        self.assertIsNotNone(rule)
        self.assertIn(theme.SURFACE, rule.group(0))
        self.assertIn("position: sticky", rule.group(0))

    def test_keyboard_focus_and_disabled_controls_have_explicit_styles(self):
        self.assertIn(":focus-visible", theme.CSS)
        self.assertIn(f"outline: 2px solid {theme.ACCENT}", theme.CSS)
        self.assertIn(":disabled", theme.CSS)


class ChartsAreDark(unittest.TestCase):
    def figure(self, cursor=3):
        doc = parse(make_document([ev("AAPL", 30, 3), ev("AAPL", 31, 2, signals=[(0, 1, "buy")]), ev("AAPL", 32, 1)],
                                  parameters={"short_window": 1, "long_window": 3}))
        model = ReplayModel(doc)
        return charts.build_figure(model, doc.strategies[0], cursor, None, model.selected_event(cursor))

    def test_backgrounds_axes_grids_legend_and_hover_labels_follow_the_palette(self):
        layout = self.figure().layout
        self.assertEqual(layout.paper_bgcolor, theme.BACKGROUND)
        self.assertEqual(layout.plot_bgcolor, theme.SURFACE)
        self.assertEqual(layout.font.color, theme.TEXT)
        self.assertEqual(layout.legend.font.color, theme.TEXT)
        self.assertEqual(layout.hoverlabel.bgcolor, theme.SURFACE_RAISED)
        self.assertEqual(layout.hoverlabel.font.color, theme.TEXT)
        self.assertEqual(layout.xaxis.gridcolor, theme.GRID)
        self.assertEqual(layout.yaxis.gridcolor, theme.GRID)
        self.assertEqual(layout.xaxis.tickfont.color, theme.TEXT_MUTED)

    def test_the_empty_state_is_dark_too(self):
        layout = self.figure(cursor=0).layout
        self.assertEqual(layout.paper_bgcolor, theme.BACKGROUND)
        self.assertEqual(layout.plot_bgcolor, theme.SURFACE)

    def test_buy_keeps_its_shape_and_its_label_not_only_its_colour(self):
        buy = next(t for t in self.figure().data if t.name == "Buy request")
        self.assertEqual(buy.marker.symbol, "triangle-up")
        self.assertEqual(list(buy.text), ["Buy"])
        self.assertEqual(buy.marker.color, theme.BUY)
        self.assertEqual(buy.textfont.color, theme.BUY)

    def test_sell_keeps_its_shape_and_its_label(self):
        doc = parse(make_document([ev("AAPL", 30, 3), ev("AAPL", 31, 2, signals=[(0, 1, "sell")])]))
        model = ReplayModel(doc)
        sell = next(t for t in charts.build_figure(model, doc.strategies[0], 2, None, model.selected_event(2)).data
                    if t.name == "Sell request")
        self.assertEqual((sell.marker.symbol, list(sell.text), sell.marker.color), ("triangle-down", ["Sell"], theme.SELL))

    def test_the_two_averages_differ_by_colour_and_by_line_style(self):
        traces = {t.name: t for t in self.figure().data}
        short, long = traces["Short SMA (1)"], traces["Long SMA (3)"]
        self.assertNotEqual(short.line.color, long.line.color)
        self.assertNotEqual(short.line.dash, long.line.dash)

    def test_the_selected_event_ring_is_visible_on_dark(self):
        ring = next(t for t in self.figure().data if t.name == "Selected event")
        self.assertEqual(ring.marker.line.color, theme.TEXT)

    def test_threshold_lines_are_labelled_and_dashed_differently(self):
        doc = parse(make_document([ev("AAPL", m, 10, indicators={"z_score": 0.1, "mean": 10}) for m in range(30, 34)],
                                  kind="mean_reversion",
                                  parameters={"lookback": 4, "entry_threshold": 1.5, "rearm_threshold": 0.5}))
        model = ReplayModel(doc)
        figure = charts.build_figure(model, doc.strategies[0], 4, None, model.selected_event(4))
        dashes = {s.line.dash for s in figure.layout.shapes if s.type == "line"}
        self.assertEqual(dashes, {"dash", "dot"})
        texts = {a.text for a in figure.layout.annotations}
        self.assertTrue({"entry +1.5", "entry -1.5", "re-arm +0.5", "re-arm -0.5"} <= texts)


class ThemeNotice(unittest.TestCase):
    """If the lab is started where its config is not read, the page says so instead of silently looking different."""

    def run_app(self, base: str):
        real = __import__("streamlit").get_option

        def fake(name):
            return base if name == "theme.base" else real(name)

        with mock.patch("streamlit.get_option", fake):
            return AppTest.from_file(str(ROOT / "app.py"), default_timeout=60).run()

    def test_no_notice_when_the_dark_theme_is_loaded(self):
        at = self.run_app("dark")
        self.assertFalse(any("dark theme settings were not loaded" in w.value for w in at.warning))

    def test_a_notice_when_it_is_not(self):
        at = self.run_app("light")
        self.assertTrue(any("dark theme settings were not loaded" in w.value for w in at.warning))


if __name__ == "__main__":
    unittest.main()
