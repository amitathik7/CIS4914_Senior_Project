"""The Strategy Lab's one dark palette, shared with the web console (web/src/styles/global.css).

Streamlit's own theme (`.streamlit/config.toml`), the custom CSS below and the Plotly charts (`charts.py`) all take their
colours from these constants. `tests/test_theme.py` checks that the config file agrees with them and that every text/background
pair is readable (WCAG 2.x contrast), so the three cannot drift apart unnoticed.

Colour is never the only carrier of meaning: Buy and Sell also differ by marker shape and text, warm-up and ignored rows by
marker shape, thresholds by dash style and label.
"""

from __future__ import annotations

from string import Template

BACKGROUND = "#131A22"         # page
SURFACE = "#18212B"            # sidebar, cards, chart plotting area
SURFACE_RAISED = "#1E2833"     # hover labels, raised controls
BORDER = "#283442"
GRID = "#222C38"
AXIS = "#364557"
TEXT = "#E2E8EF"
TEXT_MUTED = "#A3B1C1"
ACCENT = "#8EA2FF"
ACCENT_STRONG = "#4C5FD6"      # filled primary button: white text on it stays readable
BUY = "#3CC48F"
SELL = "#F06B62"
WARN = "#E5B454"

# Chart roles (drawing choices; every plotted value comes from the replay tool).
PRICE = "#C9D3DF"
SHORT_SMA = ACCENT
LONG_SMA = WARN
MEAN = "#6FC6E8"
ZSCORE = "#C4B5FD"
MEMBER_A = "#8EA2FF"
MEMBER_B = "#F0ABFC"


# ---- contrast (WCAG 2.x relative luminance) -------------------------------------------------------

def _channel(value: int) -> float:
    c = value / 255
    return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4


def luminance(color: str) -> float:
    r, g, b = (int(color[i:i + 2], 16) for i in (1, 3, 5))
    return 0.2126 * _channel(r) + 0.7152 * _channel(g) + 0.0722 * _channel(b)


def contrast_ratio(foreground: str, background: str) -> float:
    a, b = sorted((luminance(foreground), luminance(background)), reverse=True)
    return (a + 0.05) / (b + 0.05)


# ---- CSS --------------------------------------------------------------------------------------------

_CSS = Template("""
<style>
.block-container {padding-top: 1.2rem; padding-bottom: 2rem; max-width: 100%;}
h3, h4 {margin-bottom: 0.2rem;}

/* Keep the action bar (Restore defaults / Run ...) in view while the panel scrolls; browsers without :has() just scroll it. */
section[data-testid="stSidebar"] [data-testid="stLayoutWrapper"]:has(> .st-key-run_bar) {position: sticky; bottom: 0;
  z-index: 10; padding: 0.6rem 0 0.5rem; background: $surface; border-top: 1px solid $border;
  box-shadow: 0 -8px 12px -8px rgba(0, 0, 0, 0.6);}

/* Keyboard focus must be visible on the dark surfaces. */
button:focus-visible, input:focus-visible, textarea:focus-visible, [role="tab"]:focus-visible,
[role="radio"]:focus-visible, [role="slider"]:focus-visible, [data-baseweb="select"]:focus-within,
[data-testid="stExpander"] summary:focus-visible, a:focus-visible {outline: 2px solid $accent !important; outline-offset: 2px;}

/* Disabled controls stay legible (Streamlit's default dims them further). */
button:disabled, button[disabled] {opacity: 0.62 !important; cursor: not-allowed;}
input:disabled, textarea:disabled, [data-baseweb="select"] [aria-disabled="true"] {opacity: 0.7 !important;}

/* Narrow side panel: let button labels wrap instead of being cut off with an ellipsis. */
section[data-testid="stSidebar"] button {padding-left: 0.5rem; padding-right: 0.5rem; min-height: 2.5rem; height: auto;}
section[data-testid="stSidebar"] button, section[data-testid="stSidebar"] button * {white-space: normal !important;
  text-overflow: clip !important; line-height: 1.2;}

/* A long help text must not spill from the side panel over the page. */
[data-testid="stTooltipContent"] {max-width: 290px !important;}

/* The filled primary button carries dark text on the accent colour (white on it would be 2.5:1). */
button[kind="primary"], button[kind="primary"] p {color: $background !important; font-weight: 600;}

/* View navigation: the radio group drawn as three pills (the radio input stays, for keyboards and screen readers). */
.st-key-view_nav [role="radiogroup"] {gap: 0.4rem; flex-wrap: wrap;}
.st-key-view_nav [data-testid="stRadioOption"] {border: 1px solid $border; border-radius: 0.5rem; padding: 0.3rem 1rem;
  background: $surface; margin: 0;}
.st-key-view_nav [data-testid="stRadioOption"]:has(input:checked) {border-color: $accent; background: $raised; font-weight: 600;}
.st-key-view_nav [data-testid="stRadioOption"]:has(input:focus-visible) {outline: 2px solid $accent; outline-offset: 2px;}
.st-key-view_nav [data-testid="stRadioOption"] > div > div:first-child {display: none;}

/* Compare: each configuration card carries its own colour (and its label), never colour alone. */
.st-key-cmp_card_a {border-left: 4px solid $member_a !important;}
.st-key-cmp_card_b {border-left: 4px solid $member_b !important;}
</style>
""")

CSS = _CSS.substitute(background=BACKGROUND, surface=SURFACE, border=BORDER, accent=ACCENT, raised=SURFACE_RAISED, member_a=MEMBER_A,
                      member_b=MEMBER_B)


def warn_if_not_loaded() -> None:
    """Streamlit reads .streamlit/config.toml from the directory it is started in. Started from anywhere else (say the repository
    root) the dark theme would silently not apply, so say so instead of showing an unexplained light page."""
    import streamlit as st

    if st.get_option("theme.base") != "dark":
        st.warning("The Strategy Lab's dark theme settings were not loaded (Streamlit reads python/strategy_lab/.streamlit/config.toml "
                   "only when started from python/strategy_lab). Start the lab with run_lab.ps1 to get the intended look.")
