"""Start the Strategy Lab gateway that the Engine console's Strategies section talks to.

    python lab_gateway.py [--port 8765] [--exe <path to strategy_lab_replay>]

It needs only the Python standard library and the built replay tool (docs/STRATEGIES_CONSOLE.md, docs/STRATEGY_LAB.md section 4).
"""

import sys

from strategy_lab_ui.gateway import main

if __name__ == "__main__":
    sys.exit(main())
