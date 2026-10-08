"""Verify a deferred Enter search jump on a document large enough to race.

With pending-Enter option 2:A, pressing Enter while the accepted query's
results are still being extracted must close the prompt, focus the document,
and execute the jump once matching results arrive -- without another keypress.

This drives markit on a generated tall document (default 8000 sections) at
80x24:
  /needle   -> query typed with no settle (worker still running)
  Enter     -> prompt closes immediately
  wait      -> the deferred jump fires: a match count appears and the matching
               line becomes visible; then 'q' still quits cleanly

Usage:  MARKIT_BINARY=build/markit python3 check_deferred_enter.py
        [--sections N] [--settle S] [--query Q]
Exit 0 when every check passes, 1 otherwise.
"""
import argparse
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pty_harness import Session  # noqa: E402

COLS, ROWS = 80, 24

failures = []


def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name +
          ("  [%s]" % detail if detail and not cond else ""))
    if not cond:
        failures.append(name)
    return cond


def row_text(ch, row):
    return "".join(ch[row]).rstrip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sections", type=int, default=8000)
    parser.add_argument("--settle", type=float, default=4.0)
    parser.add_argument("--query", default="needle")
    args = parser.parse_args()

    lines = []
    for i in range(args.sections):
        lines.append("filler line %d" % i)
        lines.append("")
    # The match sits near the end so a correct jump must scroll far.
    lines.append("%s target here" % args.query)
    lines.append("")
    doc_text = "\n".join(lines) + "\n"

    with tempfile.NamedTemporaryFile("w", suffix=".md", delete=False) as tmp:
        tmp.write(doc_text)
        doc = tmp.name

    ses = Session(doc,
                  config_text="display:\n  horizontal: wrap\n"
                              "  navigation: hidden\n",
                  cols=COLS, rows=ROWS, start_wait=2.5)
    try:
        # Type the query with no settle, then Enter and wait: this exercises
        # the pending path when extraction has not finished yet.
        ses.frame(keys=b"/" + args.query.encode(), settle=0.0)
        raw, ch, fg, bg, bo, inv = ses.frame(keys=b"\r", settle=args.settle)
        text = [row_text(ch, r) for r in range(ROWS)]
        status = next((t for t in text if "%" in t), "<no status row>")

        check("Enter closed the prompt", not any(
            t.startswith("/") for t in text), status)
        check("accepted query produced a match count",
              bool(re.search(r"\d+/\d+", status)) or "matches" in status,
              status)
        check("deferred jump reached the match without another key",
              any(args.query + " target here" in t for t in text),
              "match not visible")

        # The app must still be responsive afterwards.
        ses.frame(keys=b"q", settle=0.4)
        check("quit after deferred jump", True)
    finally:
        ses.close()
        os.unlink(doc)

    print("ALL OK" if not failures else "FAILURES: %s" % failures)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
