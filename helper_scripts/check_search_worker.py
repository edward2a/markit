"""Exercise stale search generations across resize, mode changes, and quit.

Usage: python3 helper_scripts/check_search_worker.py
Exit 0 when the worker workload exits cleanly without hanging or terminating
by signal.
"""
import os
import signal
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pty_harness import Session, set_winsize  # noqa: E402


def main():
    doc_lines = []
    for i in range(700):
        doc_lines += [
            "# Section %d" % i,
            "",
            "needle %d with enough text to wrap across the viewport" % i,
            "",
        ]
    with tempfile.NamedTemporaryFile("w", suffix=".md", delete=False) as tmp:
        tmp.write("\n".join(doc_lines) + "\n")
        doc = tmp.name

    ses = Session(doc, cols=80, rows=24, start_wait=0.5)
    try:
        # Keep replacing the query while the first extraction is in flight.
        ses.snapshot(keys=b"/needle", settle=0.05, timeout=0.2)
        set_winsize(ses.fd, 110, 30)
        ses.snapshot(keys=b"w", settle=0.05, timeout=0.2)
        set_winsize(ses.fd, 70, 22)
        ses.snapshot(keys=b"/need" + b"[" + b"needle", settle=0.05,
                     timeout=0.2)
        time.sleep(0.5)
        ses.snapshot(timeout=0.2)
        os.write(ses.fd, b"q")

        deadline = time.time() + 5.0
        status = None
        while time.time() < deadline:
            done, status = os.waitpid(ses.pid, os.WNOHANG)
            if done:
                break
            time.sleep(0.05)
        if status is None or not os.WIFEXITED(status):
            if status is None:
                os.kill(ses.pid, signal.SIGKILL)
                os.waitpid(ses.pid, 0)
            raise RuntimeError("markit did not exit cleanly after worker churn")
        if os.WEXITSTATUS(status) != 0:
            raise RuntimeError("markit exited with status %d" % os.WEXITSTATUS(status))
    finally:
        ses.close()
        os.unlink(doc)
    print("search worker generation/resize/quit: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
