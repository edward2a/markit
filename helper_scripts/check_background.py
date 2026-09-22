"""Verify theme.background paints the whole window end to end.

Drives markit at 80x24 and checks, on a plain paragraph row:
  background: red  -> every cell of the row has SGR bg 41
  (default)        -> every cell of the row has bg None (terminal bg)
  background: none -> every cell of the row has bg None (terminal bg)

Usage:  python3 check_background.py [--dump]  (run from repo root)
Exit 0 when every check passes, 1 otherwise.
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pty_harness import Session  # noqa: E402

COLS, ROWS = 80, 24

DOC = "plain background probe line\n\n# Head\n\nmore text\n"

BASE = "display:\n  horizontal: wrap\n  navigation: hidden\n"
CONFIG_RED = BASE + "theme:\n  background: red\n"
CONFIG_NONE = BASE + "theme:\n  background: none\n"

failures = []


def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name +
          ("  [%s]" % detail if detail and not cond else ""))
    if not cond:
        failures.append(name)
    return cond


def row_text(ch, row):
    return "".join(ch[row]).rstrip()


def dump(tag, ch):
    print("--- frame %s ---" % tag)
    for r in range(len(ch)):
        print("%2d|%s" % (r, row_text(ch, r)))


def probe_row_bg(config_text, show):
    with tempfile.NamedTemporaryFile("w", suffix=".md",
                                     delete=False) as tmp:
        tmp.write(DOC)
        doc = tmp.name
    ses = Session(doc, config_text=config_text, cols=COLS, rows=ROWS)
    try:
        raw, ch, fg, bg, bo, inv = ses.frame(keys=b"\x1b[H")
        if show:
            dump("bg", ch)
        target = -1
        for r in range(ROWS):
            if "plain background probe line" in row_text(ch, r):
                target = r
                break
        if target < 0:
            return None
        return [bg[target][c] for c in range(COLS)]
    finally:
        ses.close()
        os.unlink(doc)


def main():
    show = "--dump" in sys.argv

    cells = probe_row_bg(CONFIG_RED, show)
    check("probe row found (red config)", cells is not None)
    if cells is not None:
        check("background: red paints the row (SGR 41)",
              all(v == 41 for v in cells), repr(set(cells)))

    cells = probe_row_bg(None, show)
    check("probe row found (default config)", cells is not None)
    if cells is not None:
        check("default config keeps the terminal bg (None)",
              all(v is None for v in cells), repr(set(cells)))

    cells = probe_row_bg(CONFIG_NONE, show)
    check("probe row found (none config)", cells is not None)
    if cells is not None:
        check("background: none keeps the terminal bg (None)",
              all(v is None for v in cells), repr(set(cells)))

    if failures:
        print("FAILURES: %s" % ", ".join(failures))
        return 1
    print("ALL OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
