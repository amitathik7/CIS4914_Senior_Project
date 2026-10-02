"""Runs the Strategy Lab process-level tests (stdlib unittest; no pip installs).

    python tests/strategy_lab/run_tests.py --exe <strategy_lab_replay> --probe <strategy_lab_json_probe>
                                            --fixtures tests/fixtures/strategy_lab

CTest invokes it with the built executables; the paths can also come from the environment
(STRATEGY_LAB_EXE, STRATEGY_LAB_JSON_PROBE, STRATEGY_LAB_FIXTURES). Exit status is unittest's.
"""

from __future__ import annotations

import argparse
import os
import sys
import unittest
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--probe", required=True)
    parser.add_argument("--fixtures", required=True)
    args = parser.parse_args()

    os.environ["STRATEGY_LAB_EXE"] = str(Path(args.exe).resolve())
    os.environ["STRATEGY_LAB_JSON_PROBE"] = str(Path(args.probe).resolve())
    os.environ["STRATEGY_LAB_FIXTURES"] = str(Path(args.fixtures).resolve())

    here = Path(__file__).resolve().parent
    sys.path.insert(0, str(here))
    suite = unittest.defaultTestLoader.discover(str(here), pattern="test_*.py", top_level_dir=str(here))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    # Under CTest every path is supplied, so a skipped test means something is wrong (a wrong path, a stale
    # build): that must FAIL, never pass vacuously.
    if result.skipped:
        print(f"error: {len(result.skipped)} test(s) were skipped; refusing to report success", file=sys.stderr)
        return 1
    return 0 if result.wasSuccessful() and result.testsRun > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
