"""Stress worker renders against redraw, resize, navigation and real suspension.

Usage: MARKIT_BINARY=build/markit python3 helper_scripts/check_render_lifecycle.py
Use an instrumented binary and --timeout 60 for TSan. Raw stdout, stderr,
debug events and metrics are retained in the printed artifact directory.
Latency/CPU measurements are informational; only liveness, actual SIGTSTP
stops, worker churn, clean exit and absence of TSan diagnostics are required.
Search completion/counts are deliberately not asserted (PLAN_REVIEW finding 9).
"""

import argparse
import fcntl
import json
import os
from pathlib import Path
import pty
import re
import select
import signal
import tempfile
import termios
import time

from pty_harness import BINARY, parse_grid, send, set_winsize


def spawn_supervised(argv, stderr_path):
    """Keep a parent in the app's session so SIGTSTP is not orphan-discarded."""
    master, slave = pty.openpty()
    set_winsize(slave, 100, 28)
    reports, writer = os.pipe()
    supervisor = os.fork()
    if supervisor == 0:
        os.close(master)
        os.close(reports)
        try:
            os.setsid()
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
            ready, release = os.pipe()
            child = os.fork()
            if child == 0:
                os.close(writer)
                os.close(release)
                os.setpgid(0, 0)
                os.read(ready, 1)
                os.close(ready)
                os.dup2(slave, 0)
                os.dup2(slave, 1)
                os.close(slave)
                with open(stderr_path, "wb", buffering=0) as stderr:
                    os.dup2(stderr.fileno(), 2)
                try:
                    os.execv(argv[0], argv)
                except OSError as error:
                    os.write(2, (str(error) + "\n").encode())
                    os._exit(99)
            os.close(ready)
            os.setpgid(child, child)
            signal.signal(signal.SIGTTOU, signal.SIG_IGN)
            os.tcsetpgrp(slave, child)
            os.write(writer, ("pid %d\n" % child).encode())
            os.write(release, b"1")
            os.close(release)
            while True:
                _, status = os.waitpid(child, os.WUNTRACED | os.WCONTINUED)
                if os.WIFSTOPPED(status):
                    event = "stop %d" % os.WSTOPSIG(status)
                elif os.WIFCONTINUED(status):
                    event = "continue"
                elif os.WIFEXITED(status):
                    event = "exit %d" % os.WEXITSTATUS(status)
                else:
                    event = "signal %d" % os.WTERMSIG(status)
                os.write(writer, (event + "\n").encode())
                if os.WIFEXITED(status) or os.WIFSIGNALED(status):
                    break
        except BaseException as error:
            os.write(writer, ("supervisor-error %s\n" % error).encode())
        os._exit(0)
    os.close(slave)
    os.close(writer)
    return master, reports, supervisor


class Run:
    def __init__(self, argv, artifacts, timeout):
        self.artifacts, self.timeout = artifacts, timeout
        self.output = open(artifacts / "stdout.raw", "wb", buffering=0)
        self.fd, self.reports, self.supervisor = spawn_supervised(
            argv, artifacts / "stderr.raw")
        self.report_bytes = b""
        self.events, self.raw = [], bytearray()
        # Count worker flights by thread-set transitions, not distinct thread
        # IDs: the OS reuses a finished worker's ID for the next one, so a set
        # of IDs would stay at size 1 across repeated churn.
        self.worker_flights = 0
        self.worker_active = False
        self.pid = None
        self.baseline_threads = set()
        self.wait(lambda: self.pid is not None, "supervisor startup")

    def pump(self, duration=0.02):
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            if self.pid is not None and self.baseline_threads:
                extra = self.threads() - self.baseline_threads
                if extra and not self.worker_active:
                    self.worker_flights += 1
                    self.worker_active = True
                elif not extra:
                    self.worker_active = False
            for fd in select.select([self.fd, self.reports], [], [],
                                    min(0.01, max(0, deadline - time.monotonic())))[0]:
                try:
                    chunk = os.read(fd, 65536)
                except OSError:
                    chunk = b""
                if fd == self.fd:
                    self.output.write(chunk)
                    self.raw.extend(chunk)
                else:
                    self.report_bytes += chunk
                    while b"\n" in self.report_bytes:
                        line, self.report_bytes = self.report_bytes.split(b"\n", 1)
                        event = line.decode("utf-8", "replace")
                        self.events.append(event)
                        if event.startswith("pid "):
                            self.pid = int(event.split()[1])
                if not chunk:
                    time.sleep(0.005)

    def wait(self, predicate, description):
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            self.pump()
            if predicate():
                return
            if any(e.startswith(("exit ", "signal ", "supervisor-error"))
                   for e in self.events):
                raise RuntimeError("%s: %s" % (description, self.events))
        raise RuntimeError("timed out waiting for " + description)

    def debug(self):
        return (self.artifacts / "debug.log").read_text(errors="replace")

    def threads(self):
        try:
            return {p.name for p in Path("/proc/%d/task" % self.pid).iterdir()}
        except FileNotFoundError:
            return set()

    def repaint(self):
        # First repaint bytes, not terminal presentation latency.
        self.pump(0.15)
        offset, debug_offset = len(self.raw), len(self.debug())
        started = time.monotonic()
        send(self.fd, b"j")
        self.wait(lambda: "scroll:" in self.debug()[debug_offset:] and
                  re.search(rb"\x1b\[[0-9;]*[mHfABCDGK]", self.raw[offset:]),
                  "input-driven repaint")
        return (time.monotonic() - started) * 1000

    def suspend(self, external):
        offset = len(self.events)
        started = time.monotonic()
        if external:
            os.kill(self.pid, signal.SIGTSTP)
        else:
            send(self.fd, b"\x1a")
        self.wait(lambda: any(e.startswith("stop ") for e in self.events[offset:]),
                  "external SIGTSTP stop" if external else "Ctrl+Z stop")
        stop_ms = (time.monotonic() - started) * 1000
        if "stop %d" % signal.SIGTSTP not in self.events[offset:]:
            raise RuntimeError("unexpected stop signal: %s" % self.events[offset:])
        self.pump(0.1)
        os.kill(self.pid, signal.SIGCONT)
        self.wait(lambda: "continue" in self.events[offset:], "SIGCONT")
        return stop_ms, self.repaint()

    def cpu_ticks(self):
        fields = Path("/proc/%d/stat" % self.pid).read_text().rsplit(")", 1)[1].split()
        return int(fields[11]) + int(fields[12])

    def idle_cpu(self, seconds):
        before, started = self.cpu_ticks(), time.monotonic()
        self.pump(seconds)
        return ((self.cpu_ticks() - before) / os.sysconf("SC_CLK_TCK") /
                (time.monotonic() - started) * 100)

    def close(self):
        if self.pid and not any(e.startswith(("exit ", "signal ")) for e in self.events):
            try:
                os.kill(self.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        # Supervisor reaps the app even on failure, including a stopped app.
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            self.pump()
            done, _ = os.waitpid(self.supervisor, os.WNOHANG)
            if done:
                break
        else:
            os.kill(self.supervisor, signal.SIGKILL)
            os.waitpid(self.supervisor, 0)
        self.pump(0.05)
        self.output.close()
        os.close(self.fd)
        os.close(self.reports)
        (self.artifacts / "waitpid.log").write_text("\n".join(self.events) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rounds", type=int, default=4)
    parser.add_argument("--sections", type=int, default=500)
    parser.add_argument("--timeout", type=float, default=15)
    parser.add_argument("--idle-seconds", type=float, default=1)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    if min(args.rounds, args.sections, args.timeout, args.idle_seconds) <= 0:
        parser.error("rounds, sections, timeout and idle-seconds must be positive")
    artifacts = args.output_dir or Path(tempfile.mkdtemp(prefix="markit-lifecycle-"))
    artifacts.mkdir(parents=True, exist_ok=True)
    print("artifacts: %s" % artifacts, flush=True)
    binary = os.path.abspath(BINARY)
    metrics = {"binary": binary, "sections": args.sections, "rounds": args.rounds}
    run, failure = None, None
    try:
        metrics["binary_mtime_ns"] = os.stat(binary).st_mtime_ns
        with tempfile.TemporaryDirectory(prefix="markit-lifecycle-fixture-") as tmp:
            doc, config = Path(tmp) / "stress.md", Path(tmp) / "config.yml"
            doc.write_text("".join(
                "# Section %d\n\nneedle %d **styled** prose with `inline code` "
                "and enough repeated words to wrap around a narrow viewport.\n\n"
                "> needle quoted text with more words for clipping\n\n"
                "```\nneedle code with a long tail %s\n```\n\n" % (i, i, "x" * 100)
                for i in range(args.sections)))
            config.write_text("display:\n  horizontal: wrap\n  navigation: visible\n"
                              "theme:\n  background: red\n")
            metrics["document_bytes"] = doc.stat().st_size
            run = Run([binary, "--config", str(config), "--debug",
                       str(artifacts / "debug.log"), str(doc)], artifacts, args.timeout)
            run.wait(lambda: b"wrap" in run.raw and b"Outline" in run.raw,
                     "initial UI")
            run.pump(0.5)
            if not any(cell == 41 for row in parse_grid(run.raw, 100, 28)[2]
                       for cell in row):
                raise RuntimeError("configured background was not painted")
            run.baseline_threads = run.threads()
            baseline_thread_count = len(run.baseline_threads)
            metrics["idle_cpu_before_pct"] = run.idle_cpu(args.idle_seconds)
            metrics["input_to_repaint_ms"] = run.repaint()
            started = time.monotonic()
            for i in range(args.rounds):
                # Enter closes the prompt before w, navigation or q. No search
                # adoption assertion or extra wake-up contract is needed here.
                send(run.fd, b"/needlex\x7f\r")
                run.pump(0.02)
                set_winsize(run.fd, 72 + (i % 2) * 40, 22 + (i % 2) * 8)
                send(run.fd, b"w\x0e\x0e\tjj\r\tj")
                run.pump(0.08)
            run.wait(lambda: run.debug().count("mode:") >= args.rounds and
                     run.debug().count("nav:") >= args.rounds * 2 and
                     run.debug().count("goto:") >= args.rounds,
                     "all queued stress actions")
            metrics["churn_ms"] = (time.monotonic() - started) * 1000
            # Count before suspension: Install() recreates FTXUI's own threads.
            metrics["observed_worker_flights"] = run.worker_flights
            if run.worker_flights < 2:
                raise RuntimeError("did not observe repeated worker churn; increase --sections")
            metrics["suspend_ms"], metrics["resume_input_to_repaint_ms"] = {}, {}
            for external in (False, True):
                run.wait(lambda: len(run.threads()) == baseline_thread_count,
                         "worker teardown before next suspension")
                run.baseline_threads = run.threads()
                set_winsize(run.fd, 91 if external else 83, 26)
                send(run.fd, b"/needle\r")
                run.wait(lambda: bool(run.threads() - run.baseline_threads),
                         "search extraction flight before suspension")
                stop_ms, repaint_ms = run.suspend(external)
                label = "SIGTSTP" if external else "Ctrl+Z"
                metrics["suspend_ms"][label] = stop_ms
                metrics["resume_input_to_repaint_ms"][label] = repaint_ms
            run.pump(0.5)
            metrics["idle_cpu_after_pct"] = run.idle_cpu(args.idle_seconds)
            debug = run.debug()
            for event in ("mode:", "nav:", "focus:", "goto:", "scroll:"):
                if event not in debug:
                    raise RuntimeError("missing exercised UI action: " + event)
            # Invalidate layout and observe a new flight before requesting quit.
            run.wait(lambda: len(run.threads()) == baseline_thread_count,
                     "worker teardown before quit flight")
            run.baseline_threads = run.threads()
            set_winsize(run.fd, 97, 25)
            send(run.fd, b"/needle\r")
            run.wait(lambda: bool(run.threads() - run.baseline_threads),
                     "search extraction flight before quit")
            started = time.monotonic()
            send(run.fd, b"q")
            run.wait(lambda: any(e.startswith(("exit ", "signal ")) for e in run.events),
                     "bounded quit")
            metrics["quit_ms"] = (time.monotonic() - started) * 1000
            if "exit 0" not in run.events:
                raise RuntimeError("unclean exit: %s" % run.events)
    except (OSError, RuntimeError) as error:
        failure = str(error)
    finally:
        if run is not None:
            run.close()
        diagnostics = b"".join(path.read_bytes() for path in
                               (artifacts / "stdout.raw", artifacts / "stderr.raw")
                               if path.exists())
        if b"ThreadSanitizer" in diagnostics:
            failure = "ThreadSanitizer diagnostics captured" + (
                "; " + failure if failure else "")
        metrics["result"] = "FAIL" if failure else "PASS"
        metrics["failure"] = failure
        (artifacts / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    print(json.dumps(metrics, indent=2))
    print("render lifecycle: " + metrics["result"])
    return 1 if failure else 0


if __name__ == "__main__":
    raise SystemExit(main())
