"""A stand-in for strategy_lab_replay used only by test_bridge.py.

The bridge always starts its tool as `<command> @args.txt` from a private directory; the first line of args.txt picks the
behaviour here. Like the real tool it writes UTF-8 bytes with LF line ends, never Windows text mode.
"""

import json
import os
import sys
import time

lines = open("args.txt", encoding="utf-8").read().split("\n")
mode = lines[0]


def out(text: str) -> None:
    sys.stdout.buffer.write(text.encode("utf-8"))
    sys.stdout.buffer.flush()


def err(text: str) -> None:
    sys.stderr.buffer.write(text.encode("utf-8"))
    sys.stderr.buffer.flush()


def error_document(status: int, message: str = "bad", problems=()) -> str:
    return json.dumps({"schema": "strategy_lab.error", "schema_version": "1.0", "error": {
        "code": "x", "exit_status": status, "message": message, "total_problems": len(problems),
        "problems": list(problems)}}) + "\n"


if mode == "sleep":
    time.sleep(60)
elif mode == "flood":
    chunk = b"x" * 65536
    for _ in range(400):
        sys.stdout.buffer.write(chunk)
        sys.stdout.buffer.flush()
elif mode == "exit7":
    sys.exit(7)
elif mode == "badjson":
    out("this is not json\n")
elif mode == "err2":
    out(error_document(2, "configuration error: long_window must be greater than short_window"))
    sys.exit(2)
elif mode == "err3":
    out(error_document(3, "dataset 'dataset.csv' is not valid",
                       [{"line": 3, "column": "price", "message": "price 'abc' is not a number"}]))
    sys.exit(3)
elif mode == "err4":
    out(error_document(4, "internal"))
    sys.exit(4)
elif mode == "err_mismatch":
    out(error_document(3))
    sys.exit(2)
elif mode == "err_but_zero":
    out(error_document(2))
elif mode == "exit3_text":
    out("plain text, no document\n")
    sys.exit(3)
elif mode == "echo":
    out(json.dumps({"cwd": os.getcwd(), "files": sorted(os.listdir(".")), "args": lines}, ensure_ascii=False) + "\n")
elif mode == "stderr":
    err("e" * (3 * 1024 * 1024))
    out("{}\n")
elif mode == "stderr_text":
    err("héllo 日本\n")
    out("{}\n")
elif mode == "replay":                      # write back a canned file named by the next line
    sys.stdout.buffer.write(open(lines[1], "rb").read())
