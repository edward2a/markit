"""Verify configurable keybindings end to end.

Drives markit with a remapped config at 80x24 and checks:
  s         -> opens the search prompt (search_open remapped from / to s)
  /         -> no prompt (unbound once search_open moves to s)
  Esc       -> closes the prompt (default search_cancel kept)
  x         -> scrolls down one row (scroll_down remapped from j to x)
  j         -> no movement (unbound once scroll_down moves to x)
Plus fail-fast config errors via plain subprocess (no tty needed — the app
exits during config load):
  scroll_up: [j]       -> non-zero exit, "already bound to 'scroll_down'"
  scroll_sideways: [x] -> non-zero exit, "unknown action"
  scroll_up: [f13]     -> non-zero exit, "unknown key"

Usage:  python3 check_keybindings.py [--dump]  (run from repo root)
Exit 0 when every check passes, 1 otherwise.
"""
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pty_harness import BINARY, Session  # noqa: E402

COLS, ROWS = 80, 24
ESC = b"\x1b"

DOC_LINES = ["preamble line one", "", "# SecA", ""]
DOC_LINES += [l for i in range(1, 30) for l in ("filler line %d" % i, "")]
DOC_LINES += ["trailer line", ""]
DOC = "\n".join(DOC_LINES) + "\n"

CONFIG = ("display:\n  horizontal: wrap\n  navigation: hidden\n"
          "keybindings:\n  search_open: [s]\n  scroll_down: [x]\n")

failures = []


def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name +
          ("  [%s]" % detail if detail and not cond else ""))
    if not cond:
        failures.append(name)
    return cond


def row_text(ch, row):
    return "".join(ch[row]).rstrip()


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


def position_of(status):
    """(current, total) from a status line, or None."""
    m = re.search(r"(\d+)/(\d+)\s+(\d+%)\s+(wrap|scroll)", status)
    return (m.group(1), m.group(2)) if m else None


def dump(tag, ch):
    print("--- frame %s ---" % tag)
    for r in range(len(ch)):
        print("%2d|%s" % (r, row_text(ch, r)))


def run_config_error(config_text):
    with tempfile.NamedTemporaryFile("w", suffix=".yml",
                                     delete=False) as cfg:
        cfg.write(config_text)
        cfg_path = cfg.name
    with tempfile.NamedTemporaryFile("w", suffix=".md",
                                     delete=False) as doc:
        doc.write("hi\n")
        doc_path = doc.name
    try:
        proc = subprocess.run([BINARY, "--config", cfg_path, doc_path],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=30)
        return proc.returncode, proc.stderr.decode("utf-8", "replace")
    finally:
        os.unlink(cfg_path)
        os.unlink(doc_path)


def main():
    show = "--dump" in sys.argv
    with tempfile.NamedTemporaryFile("w", suffix=".md",
                                     delete=False) as tmp:
        tmp.write(DOC)
        doc = tmp.name
    ses = Session(doc, config_text=CONFIG, cols=COLS, rows=ROWS)
    try:
        raw, ch, fg, bo, inv = ses.frame(keys=b"\x1b[H")
        if show:
            dump("home", ch)

        def status():
            r = status_row(ch)
            return row_text(ch, r) if r >= 0 else "<no status row>"

        top = position_of(status())

        raw, ch, fg, bo, inv = ses.frame(keys=b"s")
        if show:
            dump("s-opens", ch)
        check("remapped search_open (s) opens the prompt",
              prompt_row(ch) >= 0)

        raw, ch, fg, bo, inv = ses.frame(keys=b"filler" + ESC)
        if show:
            dump("typed-esc", ch)
        check("typed query counts, Esc closes (default cancel kept)",
              prompt_row(ch) < 0 and "matches" in status(), status())

        raw, ch, fg, bo, inv = ses.frame(keys=b"/")
        if show:
            dump("slash", ch)
        check("unbound / opens nothing", prompt_row(ch) < 0)

        raw, ch, fg, bo, inv = ses.frame(keys=b"x")
        after_x = position_of(status())
        check("remapped scroll_down (x) moves one row",
              top is not None and after_x is not None and
              int(after_x[0]) == int(top[0]) + 1 and
              after_x[1] == top[1], status())

        raw, ch, fg, bo, inv = ses.frame(keys=b"j")
        after_j = position_of(status())
        check("unbound j moves nothing",
              after_j is not None and after_x is not None and
              after_j == after_x, status())
    finally:
        ses.close()
        os.unlink(doc)

    rc, err = run_config_error("keybindings:\n  scroll_up: [j]\n")
    check("same-context duplicate fails fast",
          rc != 0 and "already bound to 'scroll_down'" in err, err.strip())
    rc, err = run_config_error("keybindings:\n  scroll_sideways: [x]\n")
    check("unknown action fails fast",
          rc != 0 and "unknown action" in err, err.strip())
    rc, err = run_config_error("keybindings:\n  scroll_up: [f13]\n")
    check("unknown key fails fast",
          rc != 0 and "unknown key" in err, err.strip())

    if not show and failures:
        print("rerun with --dump to see the frames")
    print("ALL OK" if not failures else "FAILURES: %s" % failures)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
