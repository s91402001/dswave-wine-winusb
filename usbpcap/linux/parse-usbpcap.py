#!/usr/bin/env python3
"""Parse a USBPcap .pcap file (no Wireshark/tshark required).

USBPcap pseudo-header (little endian, packed):
    u16 headerLen
    u64 irpId
    u32 status
    u16 function
    u8  info          bit0 = direction (1 = IN)
    u16 bus
    u16 device
    u8  endpoint
    u8  transfer      0=isoch, 1=interrupt, 2=control, 3=bulk
    u32 dataLength    (only when headerLen >= 27)

Usage:
    python3 parse-usbpcap.py output.pcap --summary
    python3 parse-usbpcap.py output.pcap --csv /tmp/usb.csv
    python3 parse-usbpcap.py output.pcap --controls        # 只印控制命令
    python3 parse-usbpcap.py output.pcap --bulk            # 只印 bulk 摘要
"""
import argparse
import csv
import struct
import sys

XFER = {0: "isoc", 1: "interrupt", 2: "control", 3: "bulk"}


def packets(path):
    with open(path, "rb") as f:
        global_header = f.read(24)
        if len(global_header) < 24:
            return
        magic = struct.unpack("<I", global_header[:4])[0]
        if magic not in (0xA1B2C3D4, 0xA1B23C4D):
            raise SystemExit(f"not a little-endian pcap: magic={magic:#x}")
        while True:
            ph = f.read(16)
            if len(ph) < 16:
                return
            ts_sec, ts_usec, incl, _orig = struct.unpack("<IIII", ph)
            data = f.read(incl)
            if len(data) < incl:
                return
            yield ts_sec + ts_usec / 1e6, data


def parse_usbpcap(data):
    if len(data) < 23:
        return None
    header_len, irp_id = struct.unpack_from("<HQ", data, 0)
    if header_len < 23 or header_len > len(data):
        return None
    status, function = struct.unpack_from("<IH", data, 10)
    info = data[16]
    bus, device = struct.unpack_from("<HH", data, 17)
    endpoint = data[21]
    transfer = data[22]
    if header_len >= 27:
        data_len = struct.unpack_from("<I", data, 23)[0]
    else:
        data_len = max(0, len(data) - header_len)
    payload = data[header_len:header_len + data_len]
    return {
        "irp": irp_id,
        "status": status,
        "function": function,
        "info": info,
        "dir": "in" if info & 1 else "out",
        "bus": bus,
        "device": device,
        "ep": endpoint & 0x7F,
        "ep_raw": endpoint,
        "xfer": XFER.get(transfer, str(transfer)),
        "len": data_len,
        "payload": payload,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("--csv")
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--controls", action="store_true")
    ap.add_argument("--bulk", action="store_true")
    ap.add_argument("--max-controls", type=int, default=200)
    args = ap.parse_args()

    rows = []
    ctl = []
    bulk = []
    devs = {}

    for ts, data in packets(args.pcap):
        u = parse_usbpcap(data)
        if not u:
            continue
        key = (u["bus"], u["device"])
        devs[key] = devs.get(key, 0) + 1
        row = {
            "time": f"{ts:.6f}", "bus": u["bus"], "device": u["device"],
            "ep": f"0x{u['ep_raw']:02x}", "xfer": u["xfer"], "dir": u["dir"],
            "status": u["status"], "len": u["len"], "setup": "", "data": "",
        }

        if u["xfer"] == "control":
            p = u["payload"]
            if len(p) >= 8:
                bm, rq = p[0], p[1]
                wv, wi, wl = struct.unpack_from("<HHH", p, 2)
                setup = f"{bm:02x} {rq:02x} {wv:04x} {wi:04x} {wl:04x}"
                body = p[8:8 + wl]
                row["setup"] = setup
                row["data"] = body.hex()
                ctl.append((ts, u, setup, body.hex()))
        elif u["xfer"] == "bulk":
            bulk.append((ts, u))

        rows.append(row)

    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()) if rows else [])
            w.writeheader()
            w.writerows(rows)
        print(f"wrote {len(rows)} rows to {args.csv}", file=sys.stderr)

    if args.controls or args.summary:
        print("== devices seen ==")
        for (bus, dev), n in sorted(devs.items()):
            print(f"  bus {bus} dev {dev}: {n} packets")
        print(f"\n== control transfers ({len(ctl)}) ==")
        for ts, u, setup, body in ctl[:args.max_controls]:
            print(f"  t={ts:.3f} bus={u['bus']} dev={u['device']} {u['dir']:3s} "
                  f"status={u['status']:d} setup=[{setup}] data={body[:64]}")
        if len(ctl) > args.max_controls:
            print(f"  ... ({len(ctl) - args.max_controls} more)")

    if args.bulk or args.summary:
        print(f"\n== bulk transfers ({len(bulk)}) ==")
        hist = {}
        for ts, u in bulk:
            hist[(u["ep_raw"], u["dir"], u["len"], u["status"])] = \
                hist.get((u["ep_raw"], u["dir"], u["len"], u["status"]), 0) + 1
        for (ep, d, ln, st), n in sorted(hist.items(), key=lambda kv: -kv[1])[:20]:
            print(f"  ep=0x{ep:02x} {d:3s} len={ln:>8d} status={st:d}  x{n}")
        errs = [b for b in bulk if b[1]["status"] != 0]
        print(f"\n  non-zero status bulk: {len(errs)}")
        for ts, u in errs[:20]:
            print(f"    t={ts:.3f} ep=0x{u['ep_raw']:02x} {u['dir']} "
                  f"len={u['len']} status={u['status']}")


if __name__ == "__main__":
    main()
