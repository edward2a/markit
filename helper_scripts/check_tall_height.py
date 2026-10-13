"""Verify a document taller than the old 65,536-row cap keeps its complete
height and that End reaches the last row.

Drives markit on a synthetic doc whose wrapped height exceeds 65,536 rows (one
fenced code block, one row per line, so it is fast to parse) and checks the
status bar position. The old MeasureWrapHeight path truncated at 65,536; the
snapshot path must report the true total.

Usage:  python3 check_tall_height.py [--dump]   (run from repo root)
Exit 0 when every check passes, 1 otherwise.
"""
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pty_harness import Session  # noqa: E402

COLS, ROWS = 80, 24
END = b"\x1b[F"
LINES = 70000  # > 65536
DOC = "```\n" + "\n".join("line%06d" % i for i in range(LINES)) + "\n```\n"
CONFIG = "display:\n  horizontal: wrap\n  navigation: hidden\n"

failures = []


def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name +
          ("  [%s]" % detail if detail and not cond else ""))
    if not cond:
        failures.append(name)
    return cond


def row_text(ch, row):
    return "".join(ch[row]).rstrip()


def status_row(ch):
    for r in range(ROWS):
        if "%" in row_text(ch, r):
            return r
    return -1


def position_of(status):
    m = re.search(r"(\d+)/(\d+)\s+(\d+%)\s+wrap", status)
    return m.groups() if m else None


def main():
    show = "--dump" in sys.argv
    with tempfile.NamedTemporaryFile("w", suffix=".md", delete=False) as tmp:
        tmp.write(DOC)
        doc = tmp.name
    # A 70k-row layout takes a moment to prepare on the worker.
    ses = Session(doc, config_text=CONFIG, cols=COLS, rows=ROWS, start_wait=8.0)
    try:
        raw, ch, fg, bg, bo, inv = ses.frame(keys=b"\x1b[H", settle=1.5)
        if show:
            for r in range(ROWS):
                print("%2d|%s" % (r, row_text(ch, r)))
        raw, ch, fg, bg, bo, inv = ses.frame(keys=END, settle=1.5)
        if show:
            for r in range(ROWS):
                print("%2d|%s" % (r, row_text(ch, r)))
        row = status_row(ch)
        status = row_text(ch, row) if row >= 0 else "<no status row>"
        pos = position_of(status)
        check("status shows a wrap position", pos is not None, status)
        if pos is None:
            return 1
        current, total = int(pos[0]), int(pos[1])
        check("total rows exceed the old 65,536 cap", total > 65536,
              "total=%d" % total)
        check("End reaches the last row", current == total,
              "current=%d total=%d" % (current, total))
    finally:
        ses.close()
        os.unlink(doc)
    print("ALL OK" if not failures else "FAILURES: %s" % failures)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
