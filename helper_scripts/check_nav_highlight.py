"""Verify the nav-bar section highlight end to end.

Drives markit on a synthetic doc (preamble + 3 sections) at 80x24 with
the nav visible and asserts, via SGR attributes in the nav column:
  home      -> top of view is the preamble: no highlight
  jj        -> top row is "# Alpha": Alpha highlighted, others not
  +11x j    -> top row is "## Beta": Beta highlighted, others not
  End       -> top row is "# Gamma": Gamma highlighted, others not
  n, n      -> nav hides and returns with the highlight intact

Usage:  python3 check_nav_highlight.py [--dump]   (run from repo root)
Exit 0 when every check passes, 1 otherwise. --dump prints each frame's
text grid plus the nav column so failures are debuggable without reruns.
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pty_harness import Session, find_row  # noqa: E402

COLS, ROWS = 80, 24
NAV_COL = COLS - 30  # 30-column nav at the right edge
YELLOW = 33

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


def nav_rows(ch):
    return ["".join(r[NAV_COL:]).rstrip() for r in ch]


def highlighted(fg, bo, row):
    return any(fg[row][c] == YELLOW and bo[row][c]
               for c in range(NAV_COL, COLS))


def only_highlighted(ch, fg, bo, name):
    """Nav row index of `name` when it is the SOLE highlight, else None."""
    row = find_row(ch, name, NAV_COL)
    if row < 0 or not highlighted(fg, bo, row):
        return None
    if any(highlighted(fg, bo, r) for r in range(len(ch)) if r != row):
        return None
    return row


def drive_to(ses, name, max_presses, show, tag):
    """Press j until `name` is the sole nav highlight; return its nav row."""
    for _ in range(max_presses):
        raw, ch, fg, bo, _ = ses.frame(keys=b"j")
        row = only_highlighted(ch, fg, bo, name)
        if row is not None:
            if show:
                from pty_harness import row_text  # noqa
                print("--- frame %s ---" % tag)
                for r in range(len(ch)):
                    print("%2d|%s" % (r, row_text(ch[r]).rstrip()))
            return row
    raw, ch, fg, bo, _ = ses.frame()
    if show:
        print("--- frame %s (STUCK) ---" % tag)
        for r in range(len(ch)):
            print("%2d|%s" % (r, "".join(ch[r]).rstrip()))
    return None


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
        # Home repaints a static screen (the app only draws on events).
        raw, ch, fg, bo, _ = ses.frame(keys=b"\x1b[H")
        if show:
            print("--- frame home ---")
            for r in range(ROWS):
                print("%2d|%s" % (r, "".join(ch[r]).rstrip()))
        nav = nav_rows(ch)
        check("nav lists Alpha/Beta/Gamma",
              any("Alpha" in l for l in nav) and
              any("Beta" in l for l in nav) and
              any("Gamma" in l for l in nav), str(nav[:6]))
        check("preamble: no highlight",
              not any(highlighted(fg, bo, r) for r in range(ROWS)))

        ra = drive_to(ses, "Alpha", 10, show, "alpha")
        check("Alpha highlighted", ra is not None)

        rb = drive_to(ses, "Beta", 40, show, "beta")
        check("Beta highlighted", rb is not None)

        raw, ch, fg, bo, _ = ses.frame(keys=b"\x1b[F")  # End
        rg = only_highlighted(ch, fg, bo, "Gamma")
        check("Gamma highlighted at End", rg is not None,
              "navrow=%s" % rg)

        raw, ch, _, _, _ = ses.frame(keys=b"n")
        check("nav hidden",
              find_row(ch, "Gamma", NAV_COL) < 0)
        raw, ch, fg, bo, _ = ses.frame(keys=b"n")
        rg = only_highlighted(ch, fg, bo, "Gamma")
        check("highlight intact after n toggle", rg is not None)
    finally:
        ses.close()
        os.unlink(doc)
    if not show and failures:
        print("rerun with --dump to see the frames")
    print("ALL OK" if not failures else "FAILURES: %s" % failures)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
