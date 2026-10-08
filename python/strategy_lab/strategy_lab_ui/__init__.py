"""Strategy Lab UI: a local browser front end for the strategy_lab_replay executable.

The UI never decides anything. It starts the C++ replay tool, validates the JSON the tool
writes, and shows it. See docs/STRATEGY_LAB.md.
"""

UI_VERSION = "0.1.0"

# The lab's JSON schema major version this UI understands (docs/STRATEGY_LAB.md section 9).
SUPPORTED_SCHEMA_MAJOR = 1
