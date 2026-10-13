"""Measure markit's idle CPU usage over a fixed window.

Spawns markit on a representative document, lets it settle, then samples the
process CPU time (utime + stime) from /proc for `--seconds` and reports the
average percent of one core. Also prints peak RSS (VmHWM).

Usage:  python3 helper_scripts/measure_idle_cpu.py [--seconds 30] [--doc FILE]
Exit 0 always (measurement, not a pass/fail check).
"""
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pty_harness import Session  # noqa: E402

CLK = os.sysconf("SC_CLK_TCK")


def cpu_seconds(pid):
    with open("/proc/%d/stat" % pid) as f:
        parts = f.read().rsplit(")", 1)[1].split()
    # After the comm field: state is parts[0]; utime=parts[11], stime=parts[12].
    return (int(parts[11]) + int(parts[12])) / CLK


def peak_rss_kb(pid):
    try:
        with open("/proc/%d/status" % pid) as f:
            for line in f:
                if line.startswith("VmHWM:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return -1


def default_doc():
    lines = []
    for i in range(400):
        lines += ["# Section %d" % i, "",
                  "paragraph %d with searchable needle text and words" % i,
                  ""]
    return "\n".join(lines) + "\n"


def main():
    seconds = 30.0
    doc_path = None
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == "--seconds" and i + 1 < len(args):
            seconds = float(args[i + 1])
        elif a == "--doc" and i + 1 < len(args):
            doc_path = args[i + 1]

    tmp = None
    if doc_path is None:
        tmp = tempfile.NamedTemporaryFile("w", suffix=".md", delete=False)
        tmp.write(default_doc())
        tmp.close()
        doc_path = tmp.name

    ses = Session(doc_path, cols=80, rows=24, start_wait=2.5)
    try:
        t0 = time.time()
        c0 = cpu_seconds(ses.pid)
        time.sleep(seconds)
        c1 = cpu_seconds(ses.pid)
        elapsed = time.time() - t0
        pct = (c1 - c0) / elapsed * 100.0
        print("idle_cpu_percent=%.2f over %.1fs" % (pct, elapsed))
        print("peak_rss_kb=%d" % peak_rss_kb(ses.pid))
    finally:
        ses.close()
        if tmp is not None:
            os.unlink(doc_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
