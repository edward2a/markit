"""Verify the current-match search highlight end to end.

Drives markit on a synthetic doc (3x "needle", one per section) at 80x24
and checks that the live view inverts exactly the current match:
  /needle   -> status "3 matches", first match ("needle alpha") inverted
  Enter     -> "1/3", mark stays on match 1
  n / n     -> mark follows to match 2, then 3 (old rows unmarked)
  n         -> wraps to match 1, mark returns
  / + Esc   -> cancel retains the query, mark persists
  /         -> fresh prompt clears the query, no inverted cells in content
  [         -> "invalid pattern", no mark in content

Usage:  python3 check_search_highlight.py [--dump] [--scroll]
        (run from repo root)
Exit 0 when every check passes, 1 otherwise.
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pty_harness import Session  # noqa: E402

COLS, ROWS = 80, 24
ESC = b"\x1b"
CONTENT_ROWS = ROWS - 3  # viewport height: chrome takes the last 3 rows.

DOC_LINES = ["preamble line one", "", "# SecA", "", "needle alpha", ""]
DOC_LINES += [l for i in range(1, 9) for l in ("filler a%d" % i, "")]
DOC_LINES += ["## SecB", "", "needle beta", ""]
DOC_LINES += [l for i in range(1, 9) for l in ("filler b%d" % i, "")]
DOC_LINES += ["# SecC", "", "needle gamma", ""]
DOC_LINES += [l for i in range(1, 13) for l in ("filler c%d" % i, "")]
DOC_LINES += ["trailer line", ""]
DOC = "\n".join(DOC_LINES) + "\n"

CONFIG = "display:\n  horizontal: wrap\n  navigation: visible\n"

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


def status(ch):
    r = status_row(ch)
    return row_text(ch, r) if r >= 0 else "<no status row>"


def marked_cols(ch, inv, line, span=None):
    """Inverted columns overlapping `span` (default: the line) in its row.

    Returns (row, cols) or (None, []) when the line's row is absent.
    """
    span = line if span is None else span
    for r in range(CONTENT_ROWS):
        t = row_text(ch, r)
        if line not in t:
            continue
        i = t.find(span)
        if i >= 0:
            return r, [c for c in range(i, i + len(span)) if inv[r][c]]
        return r, []
    return None, []


def expect_mark(ch, inv, line, name, span="needle"):
    """The line's row exists and exactly the query span is inverted."""
    r, cols = marked_cols(ch, inv, line, span)
    if r is None:
        return check(name, False, "row with %r not visible" % line)
    t = row_text(ch, r)
    want = list(range(t.find(span), t.find(span) + len(span)))
    return check(name, cols == want,
                 "row %d inverted=%s want=%s" % (r, cols, want))


def expect_unmarked(ch, inv, needle, name):
    r, cols = marked_cols(ch, inv, needle)
    if r is None:
        return check(name, True)  # off-screen rows carry no visible mark.
    return check(name, cols == [], "row %d inverted=%s" % (r, cols))


def expect_no_marks(ch, inv, name):
    bad = [(r, c) for r in range(CONTENT_ROWS) for c in range(COLS)
           if inv[r][c]]
    return check(name, not bad, "inverted cells: %s" % bad[:8])


def main():
    show = "--dump" in sys.argv
    mode = "scroll" if "--scroll" in sys.argv else "wrap"
    config = CONFIG.replace("horizontal: wrap", "horizontal: " + mode)
    print("mode: %s" % mode)
    with tempfile.NamedTemporaryFile("w", suffix=".md",
                                     delete=False) as tmp:
        tmp.write(DOC)
        doc = tmp.name
    ses = Session(doc, config_text=config, cols=COLS, rows=ROWS)
    try:
        ses.frame(keys=b"\x1b[H")
        # Generous settle: the extraction worker must land before the mark
        # can appear.
        raw, ch, fg, bo, inv = ses.frame(keys=b"/needle", settle=2.0)
        check("incremental count before jumping",
              "3 matches" in status(ch), status(ch))
        expect_mark(ch, inv, "needle alpha", "typing marks the first match")

        raw, ch, fg, bo, inv = ses.frame(keys=b"\r")
        check("Enter accepts and sits on match 1",
              "1/3" in status(ch), status(ch))
        expect_mark(ch, inv, "needle alpha", "mark stays after Enter")

        raw, ch, fg, bo, inv = ses.frame(keys=b"n")
        check("n goes to match 2", "2/3" in status(ch), status(ch))
        expect_mark(ch, inv, "needle beta", "mark follows to match 2")
        expect_unmarked(ch, inv, "needle alpha",
                        "match 1 unmarked after n")

        raw, ch, fg, bo, inv = ses.frame(keys=b"n")
        check("n goes to match 3", "3/3" in status(ch), status(ch))
        expect_mark(ch, inv, "needle gamma", "mark follows to match 3")

        raw, ch, fg, bo, inv = ses.frame(keys=b"n")
        check("n wraps to match 1", "1/3" in status(ch), status(ch))
        expect_mark(ch, inv, "needle alpha", "mark wraps to match 1")

        raw, ch, fg, bo, inv = ses.frame(keys=b"/need" + ESC)
        check("Esc cancels but retains",
              "3 matches" in status(ch), status(ch))
        # Retained query keeps navigating from the top; the mark tracks it.
        # (The retained query here is "need", retyped before Esc.)
        raw, ch, fg, bo, inv = ses.frame(keys=b"n")
        check("retained query navigates", "1/3" in status(ch), status(ch))
        expect_mark(ch, inv, "needle alpha", "mark tracks retained query",
                    span="need")

        raw, ch, fg, bo, inv = ses.frame(keys=b"/")
        check("fresh prompt clears the counter",
              "matches" not in status(ch) and
              "pattern" not in status(ch), status(ch))
        expect_no_marks(ch, inv, "cleared query leaves no mark")

        raw, ch, fg, bo, inv = ses.frame(keys=b"[" + ESC)
        check("invalid pattern reported",
              "invalid pattern" in status(ch), status(ch))
        expect_no_marks(ch, inv, "invalid pattern leaves no mark")
    finally:
        ses.close()
        os.unlink(doc)
    print("ALL OK" if not failures else "FAILURES: %s" % failures)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
