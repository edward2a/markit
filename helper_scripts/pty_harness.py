"""Reusable pty harness for driving the markit TUI and inspecting frames.

Usage:
    from pty_harness import Session, parse_grid, find_row

    ses = Session(doc_path="notes.md", config_text="display:\\n  horizontal: wrap\\n")
    raw = ses.snapshot()            # initial frame bytes
    raw = ses.snapshot(keys=b"jj")  # send keys, wait, capture repaint
    ch, fg, bold = parse_grid(raw, ses.cols, ses.rows)
    ses.close()

Frames are full-terminal repaints: parse_grid() rebuilds per-cell
(character, SGR foreground palette code or None, bold flag) grids by
interpreting cursor addressing (H/f/G), CR/LF, and SGR 'm' sequences.
Only the LAST write to each cell in the byte stream is kept, so drain
until the app is idle before trusting a frame (snapshot() settles).
Lower-level helpers (spawn, drain, send) are exposed for scripts that
need custom flows (resizes, mid-run config swaps, raw byte dumps).
"""

import fcntl
import os
import pty
import re
import select
import signal
import struct
import tempfile
import termios
import time

HOME = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY = os.path.join(HOME, "build", "markit")


def set_winsize(fd, cols, rows):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def spawn(argv, cols, rows):
    """Fork a child on a fresh pty running argv; return (master_fd, pid)."""
    master, slave = pty.openpty()
    set_winsize(slave, cols, rows)
    pid = os.fork()
    if pid == 0:
        os.setsid()
        fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
        os.dup2(slave, 0)
        os.dup2(slave, 1)
        os.dup2(slave, 2)
        os.execv(argv[0], argv)
        os._exit(99)
    return master, pid


def drain(fd, timeout=1.0):
    """Read everything available within timeout; b"" when the app is idle."""
    out = b""
    end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                chunk = os.read(fd, 65536)
            except OSError:
                break
            if not chunk:
                break
            out += chunk
    return out


def send(fd, keys):
    """Write raw key bytes (e.g. b"jj", b"w", b"\\x1b[F" for End)."""
    os.write(fd, keys)


_SGR = re.compile(r"\[([0-9;]*)([A-Za-z])")


def parse_grid(data, cols, rows):
    """Rebuild (chars, fg, bold) cell grids from a raw capture.

    fg holds the SGR palette code (30-37, 90-97) or None for default.
    """
    text = data.decode("utf-8", "replace")
    ch = [[" "] * cols for _ in range(rows)]
    fg = [[None] * cols for _ in range(rows)]
    bo = [[False] * cols for _ in range(rows)]
    r = c = 0
    cur_fg, cur_bo = None, False
    i = 0
    while i < len(text):
        if text[i] == "\x1b" and i + 1 < len(text) and text[i + 1] == "[":
            m = _SGR.match(text, i + 1)
            if not m:
                i += 2
                continue
            params, cmd = m.group(1), m.group(2)
            i = m.end()
            if cmd == "m":
                for p in params.split(";") if params else ["0"]:
                    n = int(p) if p else 0
                    if n == 0:
                        cur_fg, cur_bo = None, False
                    elif n == 1:
                        cur_bo = True
                    elif n == 22:
                        cur_bo = False
                    elif 30 <= n <= 37 or 90 <= n <= 97:
                        cur_fg = n
                    elif n == 39:
                        cur_fg = None
            elif cmd in "Hf":
                p = params.split(";")
                r = (int(p[0]) if p[0] else 1) - 1
                c = (int(p[1]) if len(p) > 1 and p[1] else 1) - 1
            elif cmd == "G":
                c = (int(params) if params else 1) - 1
            continue
        elif text[i] == "\x1b":
            i += 2  # non-CSI escape: skip
            continue
        elif text[i] == "\r":
            c = 0
        elif text[i] == "\n":
            r += 1
        elif text[i] in "\x00\x07":
            pass
        else:
            if 0 <= r < rows and 0 <= c < cols:
                ch[r][c], fg[r][c], bo[r][c] = text[i], cur_fg, cur_bo
            c += 1
        i += 1
    return ch, fg, bo


def row_text(ch_row):
    return "".join(ch_row)


def find_row(ch, needle, col_start=0, row_end=None):
    """First row whose text at/after col_start contains needle, else -1."""
    end = len(ch) if row_end is None else row_end
    for r in range(end):
        if needle in "".join(ch[r][col_start:]):
            return r
    return -1


class Session:
    """A markit instance on a sized pty. Settles after keys before capture."""

    def __init__(self, doc_path, config_path=None, config_text=None,
                 binary=BINARY, cols=80, rows=24, start_wait=2.5):
        if config_path is None and config_text is not None:
            tmp = tempfile.NamedTemporaryFile("w", suffix=".yml",
                                              delete=False)
            tmp.write(config_text)
            tmp.close()
            config_path = tmp.name
            self._tmp_config = tmp.name
        else:
            self._tmp_config = None
        argv = [binary]
        if config_path is not None:
            argv += ["--config", config_path]
        argv += [doc_path]
        self.cols, self.rows = cols, rows
        self.fd, self.pid = spawn(argv, cols, rows)
        time.sleep(start_wait)
        drain(self.fd)  # discard startup repaint; snapshots capture fresh

    def snapshot(self, keys=None, settle=0.8, timeout=0.5):
        if keys:
            send(self.fd, keys)
            time.sleep(settle)
        return drain(self.fd, timeout)

    def frame(self, keys=None, settle=0.8, timeout=0.5):
        raw = self.snapshot(keys, settle, timeout)
        return (raw,) + parse_grid(raw, self.cols, self.rows)

    def close(self):
        try:
            send(self.fd, b"q")
            time.sleep(0.3)
            os.kill(self.pid, signal.SIGKILL)
        except (OSError, ProcessLookupError):
            pass
        if self._tmp_config is not None:
            try:
                os.unlink(self._tmp_config)
            except OSError:
                pass
