# Visualization

Offline charts for a run's performance. Reads a file the C++ engine exports --
**never** the database or the live process.

## Status

Outline only. [`plot_performance.py`](plot_performance.py) has a full function
skeleton with docstrings; every function raises `NotImplementedError`. The
export schema it will consume is undecided
([`docs/OPEN_QUESTIONS.md`](../../docs/OPEN_QUESTIONS.md)).

## Setup

```bash
python -m venv .venv
# Windows:  . .venv/Scripts/activate
# POSIX:    source .venv/bin/activate
pip install -r requirements.txt
```

## Intended usage

```bash
python plot_performance.py --input run_42_performance.json --output charts/
```

## Syntax check

```bash
python -m py_compile plot_performance.py
```
