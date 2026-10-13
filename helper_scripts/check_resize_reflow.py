"""Verify wrap-mode content re-wraps on terminal resize.

The live layout is prepared on the worker and adopted asynchronously, so a
resize must eventually replace the reported total with the re-wrapped height
(narrower viewport -> more rows) and keep End reachable. Also checks that
widening restores the original total.

Usage:  python3 check_resize_reflow.py [--dump]   (run from repo root)
Exit 0 when every check passes, 1 otherwise.
"""
import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pty_harness import Session, set_winsize  # noqa: E402

ROWS = 20
END = b"\x1b[F"
CONFIG = "display:\n  horizontal: wrap\n  navigation: hidden\n"
DOC = "# One\n\n" + "".join(
    "word%d alpha beta gamma delta epsilon zeta eta theta\n\n" % i
    for i in range(400)) + "# Two\n\ntail line\n"

failures = []


def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name +
          ("  [%s]" % detail if detail and not cond else ""))
    if not cond:
        failures.append(name)
    return cond


def status_of(ses, cols, keys=b""):
    raw = ses.snapshot(keys=keys, settle=1.0, timeout=1.0)
    from pty_harness import parse_grid
    ch, _, _, _, _ = parse_grid(raw, cols, ROWS)
    for r in range(ROWS):
        s = "".join(ch[r]).rstrip()
        if "%" in s:
            return s
    return ""


def position_of(status):
    m = re.search(r"(\d+)/(\d+)\s+(\d+%)\s+wrap", status)
    return (int(m.group(1)), int(m.group(2))) if m else None


def main():
    show = "--dump" in sys.argv
    with tempfile.NamedTemporaryFile("w", suffix=".md", delete=False) as tmp:
        tmp.write(DOC)
        doc = tmp.name
    ses = Session(doc, config_text=CONFIG, cols=80, rows=ROWS, start_wait=3.0)
    try:
        pos = position_of(status_of(ses, 80, keys=b"\x1b[H"))
        check("initial wrap position", pos is not None, "no position")
        if pos is None:
            return 1
        wide_total = pos[1]

        set_winsize(ses.fd, 40, ROWS)
        time.sleep(0.8)
        pos = position_of(status_of(ses, 40))
        check("narrowing increases the wrapped total",
              pos is not None and pos[1] > wide_total,
              "wide=%d narrow=%s" % (wide_total, pos))
        narrow_total = pos[1] if pos else -1

        pos = position_of(status_of(ses, 40, keys=END))
        check("End reaches the last row at 40 cols",
              pos is not None and pos[0] == pos[1] == narrow_total,
              "pos=%s narrow=%d" % (pos, narrow_total))

        set_winsize(ses.fd, 80, ROWS)
        time.sleep(0.8)
        pos = position_of(status_of(ses, 80))
        check("widening restores the original total",
              pos is not None and pos[1] == wide_total,
              "expected=%d got=%s" % (wide_total, pos))
    finally:
        ses.close()
        os.unlink(doc)
    print("ALL OK" if not failures else "FAILURES: %s" % failures)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
