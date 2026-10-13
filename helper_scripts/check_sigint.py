"""Verify universal Ctrl+C / SIGINT handling and terminal restoration.

The app must treat Ctrl+C as an application-wide interrupt regardless of the
search prompt or active work: it cancels, restores the terminal, and exits
within a bounded time instead of awaiting an entire stale scan. This checks:
  - external SIGINT with the prompt closed (idle)
  - external SIGINT with the prompt open and a search active
  - keyboard Ctrl+C with the prompt open and a search active
For each it asserts the process exits within --timeout and that the pty line
discipline is back in canonical/echo mode afterwards.

Usage:  MARKIT_BINARY=build/markit python3 check_sigint.py
        [--timeout S] [--sections N]
Exit 0 when every check passes, 1 otherwise.
"""
import argparse
import os
import signal
import sys
import tempfile
import termios
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pty_harness as H  # noqa: E402

failures = []


def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name +
          ("  [%s]" % detail if detail and not cond else ""))
    if not cond:
        failures.append(name)
    return cond


def wait_exit(pid, timeout):
    """Return the wait status within `timeout`, or None if still running."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        wpid, status = os.waitpid(pid, os.WNOHANG)
        if wpid == pid:
            return status
        time.sleep(0.02)
    return None


def terminal_restored(master):
    try:
        attrs = termios.tcgetattr(master)
    except termios.error:
        return False
    lflag = attrs[3]
    return bool(lflag & termios.ICANON) and bool(lflag & termios.ECHO)


def run_case(name, doc, pre_keys, deliver, timeout):
    fd, pid = H.spawn([H.BINARY], 80, 24)
    try:
        time.sleep(2.5)
        H.drain(fd)
        if pre_keys:
            H.send(fd, pre_keys)
            time.sleep(0.2)  # let the prompt/search start before interrupting
        if deliver == "keyboard":
            H.send(fd, b"\x03")
        else:
            os.kill(pid, signal.SIGINT)
        started = time.monotonic()
        status = wait_exit(pid, timeout)
        elapsed = (time.monotonic() - started) * 1000.0
        if status is None:
            check(name + ": bounded exit", False, "still running after %.0f ms"
                  % elapsed)
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
            return
        check(name + ": bounded exit", True,
              "%.0f ms" % elapsed)
        check(name + ": terminal restored", terminal_restored(fd))
        print("  exit after %.0f ms (status %d)" % (elapsed, status))
    finally:
        try:
            os.close(fd)
        except OSError:
            pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument("--sections", type=int, default=4000)
    args = parser.parse_args()

    lines = []
    for i in range(args.sections):
        lines.append("filler line %d" % i)
        lines.append("")
    lines.append("needle near the end")
    lines.append("")
    with tempfile.NamedTemporaryFile("w", suffix=".md", delete=False) as tmp:
        tmp.write("\n".join(lines) + "\n")
        doc = tmp.name

    try:
        run_case("SIGINT idle", doc, None, "signal", args.timeout)
        run_case("SIGINT active search", doc, b"/needle", "signal",
                 args.timeout)
        run_case("keyboard Ctrl+C active search", doc, b"/needle", "keyboard",
                 args.timeout)
    finally:
        os.unlink(doc)

    print("ALL OK" if not failures else "FAILURES: %s" % failures)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
