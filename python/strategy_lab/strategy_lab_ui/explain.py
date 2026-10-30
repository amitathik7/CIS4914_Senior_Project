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
    "ml_logistic": (
        "For each allowed symbol the strategy computes six price features (recent returns, a z-score, the gap between "
        "a short and a long average, and volatility) and a **logistic regression** turns them into **p**, the "
        "probability that the close is higher `horizon` bars later.\n\n"
        "- It **learns as it goes**: each prediction becomes a training sample once its outcome is known, so it never "
        "sees the future.\n"
        "- Nothing is decided until 21 bars have arrived and `min_training_samples` samples have trained it.\n"
        "- p at or above **entry_probability**: a **Buy request**; at or below **1 - entry_probability**: a **Sell "
        "request**. The request's confidence is p (or 1 - p).\n"
        "- It asks **once per excursion**, re-arming when p comes back within `rearm_band` of 0.5.\n\n"
        "There is no exit logic and no position: these are requests, not trades."),
}
