"""Dump a raw pty capture as plain text rows (no attributes).

Usage:  python3 decode.py <capture.bin> <cols> <rows>
Ported from the throwaway /tmp/opencode/decode.py so captures saved by
any harness script stay readable without re-running the app.
"""
import re
import sys


def decode_file(path, cols, rows):
    data = open(path, "rb").read().decode("utf-8", "replace")
    grid = [[" "] * cols for _ in range(rows)]
    r = c = 0
    i = 0

    def put_at(rr, cc, ch):
        if 0 <= rr < rows and 0 <= cc < cols:
            grid[rr][cc] = ch

    while i < len(data):
        ch = data[i]
        if ch == "\x1b":
            j = i + 1
            if j < len(data) and data[j] == "[":
                k = j + 1
                params = ""
                while k < len(data) and not ("@" <= data[k] <= "~"):
                    if data[k] in ";0123456789?":
                        params += data[k]
                        k += 1
                    else:
                        break
                if k < len(data):
                    cmd = data[k]
                    if cmd in "Hf":
                        p = params.split(";")
                        rr = int(p[0]) if p[0] else 1
                        cc = int(p[1]) if len(p) > 1 and p[1] else 1
                        r = rr - 1
                        c = cc - 1
                    elif cmd == "G":
                        c = (int(params) - 1) if params else 0
                    i = k + 1
                    continue
            elif j < len(data) and data[j] == "]":
                k = j + 1
                while k < len(data) and data[k] not in "\x07" and not (
                    data[k] == "\x1b"
                    and k + 1 < len(data)
                    and data[k + 1] == "\\"
                ):
                    k += 1
                if k < len(data):
                    k += 1
                    if data[k - 1] == "\x1b":
                        k += 1
                i = k
                continue
            else:
                k = j + 1
                i = k
                continue
        elif ch == "\r":
            c = 0
            i += 1
        elif ch == "\n":
            r += 1
            i += 1
        elif ch in "\x00\x07":
            i += 1
        else:
            if r < rows and c < cols:
                grid[r][c] = ch
            c += 1
            i += 1
    return grid


def main():
    cols, rows = int(sys.argv[2]), int(sys.argv[3])
    for i, row in enumerate(decode_file(sys.argv[1], cols, rows)[:30]):
        line = "".join(row).rstrip()
        if line.strip():
            n = 0
            for u in line:
                n += 1 if ord(u) < 0x2500 else 2
            print(f"{n:3d}|{line}")


if __name__ == "__main__":
    main()
