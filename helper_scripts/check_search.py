"""Verify in-document search end to end.

Drives markit on a synthetic doc (3x "needle" spread over 3 sections, tall
enough to scroll) at 80x24 and checks the less-style flow:
  /         -> prompt row opens above the action bar
  needle    -> status shows "3 matches" (unjumped, incremental)
  Enter     -> accepts: prompt closes, jumps to match 1 ("1/3" persists)
  n / n     -> "2/3", "3/3"; n wraps to "1/3"; N back to "3/3"
  / + Esc   -> cancels without jumping; the query is retained so n still
               navigates from the top
  /         -> fresh empty prompt (query cleared, counter gone)
  [         -> "invalid pattern", Enter is a position no-op
  Ctrl+N    -> nav hides/shows (Ctrl+N kept the toggle)

Usage:  python3 check_search.py [--dump] [--scroll]  (run from repo root)
Exit 0 when every check passes, 1 otherwise.
"""
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pty_harness import Session  # noqa: E402

COLS, ROWS = 80, 24
CTRL_N = b"\x0e"
ESC = b"\x1b"

# Row-finding helpers: the harness rebuilds the grid from the app's repaint
# diffs, so fixed row indices are unreliable (a repaint can land anywhere in
# the capture window). Instead locate rows by content. The synthetic doc
# below contains no "/" or "%" characters, so the prompt row (starts with
# "/") and the status row (contains "%") are unambiguous.
def prompt_row(ch):
    for r in range(ROWS):
        if row_text(ch, r).startswith("/"):
            return r
    return -1


def status_row(ch):
    for r in range(ROWS):
        if "%" in row_text(ch, r):
            return r
    return -1

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


def position_of(status):
    """(current, total, percent, mode) from a status line, or None.

    Compares normalized tokens: the flex padding between filename and
    position shifts with suffix length, and the total shifts with the
    viewport height (prompt open/closed), so raw string equality is
    meaningless across frames.
    """
    m = re.search(r"(\d+)/(\d+)\s+(\d+%)\s+(wrap|scroll)", status)
    return m.groups() if m else None


def dump(tag, ch):
    print("--- frame %s ---" % tag)
    for r in range(len(ch)):
        print("%2d|%s" % (r, row_text(ch, r)))


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
        raw, ch, fg, bo, inv = ses.frame(keys=b"\x1b[H")
        if show:
            dump("home", ch)

        def status():
            r = status_row(ch)
            return row_text(ch, r) if r >= 0 else "<no status row>"

        check("plain status, no prompt",
              "matches" not in status() and "pattern" not in status() and
              prompt_row(ch) < 0, status())

        raw, ch, fg, bo, inv = ses.frame(keys=b"/")
        if show:
            dump("slash", ch)
        check("prompt row opens", prompt_row(ch) >= 0)

        raw, ch, fg, bo, inv = ses.frame(keys=b"needle")
        if show:
            dump("typed", ch)
        check("incremental count before jumping",
              "3 matches" in status(), status())

        raw, ch, fg, bo, inv = ses.frame(keys=b"\r")
        if show:
            dump("enter", ch)
        check("Enter accepts: prompt closed, on match 1",
              "1/3" in status() and prompt_row(ch) < 0, status())

        raw, ch, fg, bo, inv = ses.frame(keys=b"n")
        check("n goes to match 2", "2/3" in status(), status())
        raw, ch, fg, bo, inv = ses.frame(keys=b"n")
        check("n goes to match 3", "3/3" in status(), status())
        raw, ch, fg, bo, inv = ses.frame(keys=b"n")
        check("n wraps to match 1", "1/3" in status(), status())
        raw, ch, fg, bo, inv = ses.frame(keys=b"N")
        if show:
            dump("N", ch)
        check("N goes back to match 3", "3/3" in status(), status())

        raw, ch, fg, bo, inv = ses.frame(keys=b"\x1b[H")
        top = status()
        raw, ch, fg, bo, inv = ses.frame(keys=b"/need" + ESC)
        if show:
            dump("esc-cancel", ch)
        check("Esc cancels without jumping, query retained",
              prompt_row(ch) < 0 and "3 matches" in status() and
              position_of(status()) is not None and
              position_of(status())[0] == position_of(top)[0] and
              position_of(status())[1] == position_of(top)[1], status())

        raw, ch, fg, bo, inv = ses.frame(keys=b"n")
        check("retained query navigates from the top",
              "1/3" in status(), status())

        raw, ch, fg, bo, inv = ses.frame(keys=b"/")
        pr = prompt_row(ch)
        # A cleared query hides the counter (the app only shows a suffix for
        # a non-empty query), which proves the fresh prompt is empty even
        # when transient repaint bytes overlay the prompt row itself.
        check("reopen starts fresh: prompt open, counter gone",
              pr >= 0 and "matches" not in status() and
              "pattern" not in status(),
              ("row %d " % pr if pr >= 0 else "") + status())

        raw, ch, fg, bo, inv = ses.frame(keys=b"[")
        if show:
            dump("invalid", ch)
        check("invalid pattern reported",
              "invalid pattern" in status(), status())
        before = position_of(status())
        raw, ch, fg, bo, inv = ses.frame(keys=b"\r")
        # The totals may shift (Enter closes the prompt, growing the viewport
        # by a row), but the current position must not move.
        after = position_of(status())
        check("Enter is a position no-op on invalid pattern",
              before is not None and after is not None and
              before[0] == after[0] and
              "invalid pattern" in status(), status())

        raw, ch, fg, bo, inv = ses.frame(keys=CTRL_N)
        if show:
            dump("ctrln", ch)
        check("Ctrl+N hides the nav",
              all("Outline" not in row_text(ch, r) for r in range(ROWS)))
        raw, ch, fg, bo, inv = ses.frame(keys=CTRL_N)
        check("Ctrl+N restores the nav",
              any("Outline" in row_text(ch, r) for r in range(ROWS)))
    finally:
        ses.close()
        os.unlink(doc)
    if not show and failures:
        print("rerun with --dump to see the frames")
    print("ALL OK" if not failures else "FAILURES: %s" % failures)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
