"""Verify nav-bar focus, keyboard cursor, and Enter-to-jump end to end.

Drives markit on a synthetic doc (preamble + 3 sections) at 80x24:
  Home  -> preamble: no highlight, nothing inverted
  Tab   -> focus enters nav: Outline title inverted, cursor synced to
           Alpha (preamble maps to first), still no view highlight
  j     -> cursor moves to Beta while the view stays put (nav consumes j)
  Enter -> main view jumps to Beta: Beta yellow AND inverted, focus stays
  End   -> cursor to Gamma; view untouched (Beta still the highlight)
  Enter -> Gamma yellow AND inverted
  Tab   -> back to main: title plain, no inverted heading rows, Gamma
           still the sole highlight

Usage:  python3 check_nav_focus.py [--dump] [--scroll]  (run from repo root)
Exit 0 when every check passes, 1 otherwise. --dump prints each frame's
text grid so failures are debuggable without reruns.
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pty_harness import Session, find_row  # noqa: E402

COLS, ROWS = 80, 24
NAV_COL = COLS - 30  # 30-column nav at the right edge
YELLOW = 33

TAB = b"\t"
RET = b"\r"
HOME = b"\x1b[H"
END = b"\x1b[F"

DOC_BODY = ["preamble line one", "", "# Alpha", ""]
DOC_BODY += [l for i in range(1, 9) for l in ("alpha filler %d" % i, "")]
DOC_BODY += ["## Beta", ""]
DOC_BODY += [l for i in range(1, 9) for l in ("beta filler %d" % i, "")]
DOC_BODY += ["# Gamma", ""]
DOC_BODY += [l for i in range(1, 21) for l in ("gamma filler %d" % i, "")]
DOC = "\n".join(DOC_BODY) + "\n"

CONFIG = "display:\n  horizontal: wrap\n  navigation: visible\n"

failures = []


def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name +
          ("  [%s]" % detail if detail and not cond else ""))
    if not cond:
        failures.append(name)
    return cond


def highlighted(fg, bo, row):
    return any(fg[row][c] == YELLOW and bo[row][c]
               for c in range(NAV_COL, COLS))


def inverted(inv, row):
    return any(inv[row][c] for c in range(NAV_COL, COLS))


def only_highlighted(ch, fg, bo, name):
    """Nav row index of `name` when it is the SOLE highlight, else None."""
    row = find_row(ch, name, NAV_COL)
    if row < 0 or not highlighted(fg, bo, row):
        return None
    if any(highlighted(fg, bo, r) for r in range(len(ch)) if r != row):
        return None
    return row


def only_cursor(ch, inv, name, title_row):
    """Nav row of `name` when it is the SOLE inverted heading row, else None.

    The focused title row is inverted too, so it is exempt.
    """
    row = find_row(ch, name, NAV_COL)
    if row < 0 or not inverted(inv, row):
        return None
    if any(inverted(inv, r) for r in range(len(ch))
           if r != row and r != title_row):
        return None
    return row


def dump(tag, ch):
    print("--- frame %s ---" % tag)
    for r in range(len(ch)):
        print("%2d|%s" % (r, "".join(ch[r]).rstrip()))


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
        raw, ch, fg, bo, inv = ses.frame(keys=HOME)
        if show:
            dump("home", ch)
        check("preamble: no highlight, nothing inverted",
              not any(highlighted(fg, bo, r) for r in range(ROWS)) and
              not any(inverted(inv, r) for r in range(ROWS)))

        raw, ch, fg, bo, inv = ses.frame(keys=TAB)
        if show:
            dump("tab", ch)
        title = find_row(ch, "Outline", NAV_COL)
        check("Tab focuses nav: title inverted",
              title >= 0 and inverted(inv, title), "titlerow=%s" % title)
        check("cursor synced to Alpha, sole inversion",
              title >= 0 and
              only_cursor(ch, inv, "Alpha", title) is not None)
        check("no view highlight yet",
              not any(highlighted(fg, bo, r) for r in range(ROWS)))

        raw, ch, fg, bo, inv = ses.frame(keys=b"j")
        if show:
            dump("j", ch)
        check("j moves nav cursor to Beta, view stays put",
              only_cursor(ch, inv, "Beta", title) is not None and
              not any(highlighted(fg, bo, r) for r in range(ROWS)))

        raw, ch, fg, bo, inv = ses.frame(keys=RET)
        if show:
            dump("enter-beta", ch)
        rb = only_highlighted(ch, fg, bo, "Beta")
        check("Enter jumps to Beta: sole highlight + cursor",
              rb is not None and inverted(inv, rb) and
              inverted(inv, title), "navrow=%s" % rb)

        raw, ch, fg, bo, inv = ses.frame(keys=END)
        if show:
            dump("end", ch)
        check("End moves cursor to Gamma, view still Beta",
              only_cursor(ch, inv, "Gamma", title) is not None and
              only_highlighted(ch, fg, bo, "Beta") is not None)

        raw, ch, fg, bo, inv = ses.frame(keys=RET)
        if show:
            dump("enter-gamma", ch)
        rg = only_highlighted(ch, fg, bo, "Gamma")
        check("Enter jumps to Gamma: sole highlight + cursor",
              rg is not None and inverted(inv, rg), "navrow=%s" % rg)

        raw, ch, fg, bo, inv = ses.frame(keys=TAB)
        if show:
            dump("tab-back", ch)
        check("Tab back: title plain, no inverted heading rows",
              not inverted(inv, title) and
              not any(inverted(inv, r) for r in range(ROWS) if r != title))
        check("Gamma still the sole highlight",
              only_highlighted(ch, fg, bo, "Gamma") is not None)
    finally:
        ses.close()
        os.unlink(doc)
    if not show and failures:
        print("rerun with --dump to see the frames")
    print("ALL OK" if not failures else "FAILURES: %s" % failures)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
