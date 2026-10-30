#!/usr/bin/env python3
"""Compare a Windows USBPcap extract with a Linux usbmon capture.

Usage:
    python3 compare.py <windows_dir_or_csv> <wine_usbmon.csv>

  <windows_dir>  directory containing control.csv / bulk.csv produced by
                 03-extract.ps1
  <wine_usbmon.csv>  CSV produced by linux/parse-usbmon.py <mon> <out.csv>

What it prints:
  * control command sequence from both sides (side by side)
  * bulk completion length histograms
  * a rough sequence diff summary
"""
import csv
import os
import sys
from difflib import SequenceMatcher


def read_csv(path):
    with open(path, newline="", errors="replace") as f:
        return list(csv.DictReader(f))


def load_windows(path):
    if os.path.isdir(path):
        ctl = read_csv(os.path.join(path, "control.csv")) if \
            os.path.exists(os.path.join(path, "control.csv")) else []
        bulk = read_csv(os.path.join(path, "bulk.csv")) if \
            os.path.exists(os.path.join(path, "bulk.csv")) else []
    else:
        rows = read_csv(path)
        ctl = [r for r in rows if r.get("usb.transfer_type") in ("0x00000002", "2")]
        bulk = [r for r in rows if r.get("usb.transfer_type") in ("0x00000003", "3")]

    controls = []
    for r in ctl:
        rt = r.get("usb.setup.bmRequestType", "")
        rq = r.get("usb.setup.bRequest", "")
        if not rt and not rq:
            continue
        key = (rt, rq, r.get("usb.setup.wValue", ""), r.get("usb.setup.wIndex", ""),
               r.get("usb.setup.wLength", ""))
        controls.append(key)

    bulks = []
    for r in bulk:
        try:
            ep = int(r.get("usb.endpoint_address", "0"), 0)
            ln = int(r.get("usb.data_len", "0") or 0)
        except ValueError:
            continue
        st = r.get("usb.urb_status", "")
        bulks.append((ep, ln, st))
    return controls, bulks


def load_usbmon(path):
    rows = read_csv(path)
    controls = [(r["setup"],) for r in rows
                if r.get("type") == "control" and r.get("event") == "S" and r.get("setup")]
    bulks = []
    for r in rows:
        if r.get("type") == "bulk" and r.get("event") == "C":
            try:
                bulks.append((int(r["ep"]), int(r["length"]), r["status"]))
            except (KeyError, ValueError):
                pass
    return controls, bulks


def hist(items, key_len=3):
    h = {}
    for it in items:
        h[it] = h.get(it, 0) + 1
    return sorted(h.items(), key=lambda kv: -kv[1])


def fmt_seq(seq, n=30):
    out = []
    for i, s in enumerate(seq[:n]):
        out.append(f"  {i:3d}  {' '.join(x for x in s if x)}")
    if len(seq) > n:
        out.append(f"  ... ({len(seq) - n} more)")
    return "\n".join(out)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    win_ctl, win_bulk = load_windows(sys.argv[1])
    lin_ctl, lin_bulk = load_usbmon(sys.argv[2])

    print("== control commands: Windows ==")
    print(fmt_seq(win_ctl))
    print("\n== control commands: Wine ==")
    print(fmt_seq(lin_ctl))

    ratio = SequenceMatcher(a=[str(x) for x in win_ctl],
                            b=[str(x) for x in lin_ctl]).ratio()
    print(f"\ncontrol sequence similarity: {ratio:.2%}")
    if len(win_ctl) != len(lin_ctl):
        print(f"count differs: windows={len(win_ctl)} wine={len(lin_ctl)}")

    print("\n== bulk completions: Windows ==")
    for k, n in hist(win_bulk)[:15]:
        print(f"  ep=0x{k[0]:02x} len={k[1]} status={k[2]}  x{n}")
    print("\n== bulk completions: Wine ==")
    for k, n in hist(lin_bulk)[:15]:
        print(f"  ep=0x{k[0]:02x} len={k[1]} status={k[2]}  x{n}")

    win_full = sum(1 for ep, ln, st in win_bulk if ln == 5005312)
    lin_full = sum(1 for ep, ln, st in lin_bulk if ln == 5005312)
    print(f"\nfull 5,005,312-byte completions: windows={win_full} wine={lin_full}")


if __name__ == "__main__":
    main()
