"""Short, fixed explanations shown next to the strategies. They describe the documented behaviour
(docs/strategies/*.md); they do not compute anything and every number shown elsewhere comes from the tool."""

HOW_IT_WORKS = {
    "sma_crossover": (
        "For each allowed symbol the strategy keeps two **simple moving averages of the close**: a short one and "
        "a long one. Nothing is decided until the long window is full (*warm-up*).\n\n"
        "- The short average moves **above** the long one: a **Buy request**.\n"
        "- It moves **below**: a **Sell request**.\n"
        "- The first time the averages differ after warm-up only sets a *baseline*; it never signals.\n"
        "- Equal averages change nothing.\n\n"
        "These are requests for a later risk stage, not trades: no position, size or profit exists here."),
    "mean_reversion": (
        "For each allowed symbol the strategy keeps the last `lookback` closes and measures how far the newest close "
        "is from their mean in standard deviations: the **z-score**.\n\n"
        "- z at or below **-entry_threshold**: a **Buy request** (price far below its recent mean).\n"
        "- z at or above **+entry_threshold**: a **Sell request**.\n"
        "- It asks **once per excursion**: it re-arms when |z| falls to `rearm_threshold` or less, or when the "
        "window goes flat.\n"
        "- Nothing is decided until the window is full (*warm-up*).\n\n"
        "There is no exit logic and no position: these are requests, not trades."),
}
