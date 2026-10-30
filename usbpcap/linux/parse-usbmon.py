#!/usr/bin/env python3
"""Parse a Linux usbmon text capture (``usbmon/8u``) into a CSV/normalized summary.

usbmon u-format columns (space separated):
    <urb> <timestamp> <S|C|E> <pipe> [s <setup>] <status> <length> [<|=|> <data>]

Examples:
    ffff... 1234567890 S Ci:8:006:0 s a3 00 0000 0001 0004 4 <
    ffff... 1234567890 C Ci:8:006:0 0 4 = 01 02 03 00
    ffff... 1234567891 S Bo:8:006:2 -115 4096 <
    ffff... 1234567892 C Bo:8:006:2 0 4096 = ...

Usage:
    python3 parse-usbmon.py /tmp/wine.mon [out.csv]
"""
import csv
import re
import sys

PIPE_RE = re.compile(r"^(?P<dir>[BC])(?P<type>[io]):(?P<bus>\d+):(?P<dev>\d+):(?P<ep>\d+)$")


def parse_line(line):
    parts = line.split()
    if len(parts) < 7:
        return None
    try:
        urb, ts, event, pipe = parts[0], parts[1], parts[2], parts[3]
    except IndexError:
        return None
    m = PIPE_RE.match(pipe)
    if not m:
        return None

    rest = parts[4:]
    setup = ""
    # S control lines: "s <bmRequestType> <bRequest> <wValue> <wIndex> <wLength> <len> <"
    if rest and rest[0] == "s":
        setup = " ".join(rest[1:6])
        rest = rest[6:]

    if event == "S":
        # status is always 0 for submissions; the next token is the length
        length = 0
        if rest and rest[0].isdigit():
            length = int(rest[0])
        status = 0
        data = ""
    elif event in ("C", "E"):
        if len(rest) < 2:
            return None
        status, length = int(rest[0]), int(rest[1])
        data = " ".join(rest[3:]) if len(rest) > 3 else ""
    else:
        return None

    return {
        "time": ts,
        "event": event,
        "dir": "in" if m.group("dir") == "C" else "out",
        "bus": m.group("bus"),
        "dev": m.group("dev"),
        "ep": int(m.group("ep")),
        "type": "control" if int(m.group("ep")) == 0 else "bulk",
        "status": status,
        "length": length,
        "setup": setup,
        "data": data,
    }


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else ""

    rows = []
    with open(src, "r", errors="replace") as f:
        for line in f:
            r = parse_line(line)
            if r:
                rows.append(r)

    if out:
        with open(out, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()) if rows else [])
            w.writeheader()
            w.writerows(rows)
        print(f"wrote {len(rows)} rows to {out}", file=sys.stderr)
        return

    # summary to stdout
    ctl = [r for r in rows if r["type"] == "control" and r["event"] == "S"]
    bulk_c = [r for r in rows if r["type"] == "bulk" and r["event"] == "C"]
    errors = [r for r in rows if r["status"] < 0]

    print(f"total urbs: {len(rows)}")
    print(f"control submissions: {len(ctl)}")
    print(f"bulk completions: {len(bulk_c)}")
    print(f"errors (status<0): {len(errors)}")
    print()

    print("== control setup sequence (first 40) ==")
    for r in ctl[:40]:
        print(f"  t={r['time']} dev={r['dev']} setup={r['setup']}")
    print()

    print("== bulk length histogram ==")
    hist = {}
    for r in bulk_c:
        key = (r["ep"], r["length"], r["status"])
        hist[key] = hist.get(key, 0) + 1
    for (ep, length, status), n in sorted(hist.items(), key=lambda kv: -kv[1])[:20]:
        print(f"  ep=0x{ep:02x} len={length} status={status}  x{n}")
    print()

    print("== bulk errors ==")
    for r in errors:
        if r["type"] == "bulk":
            print(f"  t={r['time']} dev={r['dev']} ep=0x{r['ep']:02x} "
                  f"len={r['length']} status={r['status']}")


if __name__ == "__main__":
    main()
