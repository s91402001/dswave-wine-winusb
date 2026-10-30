# Diagnostic tools

Standalone programs that talk to the DSCope M454 directly through
`libusb-1.0`, without Wine and without DSWave. They are the fastest way to tell
whether a problem is in the device, the kernel driver or the Wine shim.

Build:

```bash
gcc -O2 $(pkg-config --cflags libusb-1.0) -o dswbulk dswbulk.c $(pkg-config --libs libusb-1.0)
gcc -O2 $(pkg-config --cflags libusb-1.0) -o dswfpga dswfpga.c $(pkg-config --libs libusb-1.0)
```

> Stop the helper and DSWave first: both claim interface 0.

---

## dswbulk — bulk IN soak test

```bash
./dswbulk [seconds] [mbps] [chunk] [poll84]
```

| Argument | Default | Meaning |
|---|---|---|
| `seconds` | 30 | run time |
| `mbps` | 20 | pacing cap (0 = unlimited) |
| `chunk` | 405504 | libusb request size; frames are 5,005,312 bytes |
| `poll84` | 0 | 1 = also poll endpoint 0x84 in a thread |

It resets the device, claims interface 0, sends the capture-start sequence
(`03 01 00`, `02 00 00`, `01 01 00`) and reads until an error or the time limit.

Typical good result:

```
t=5.01s total=105.0MB avg=20.0MB/s frames=20
done: 5.008s 105025536 bytes 259 pkts
```

An error such as `-1 LIBUSB_ERROR_IO` (`-EPROTO` at the URB level) points at
the device/link, not at the shim.

---

## dswfpga — replay the FPGA bitstream upload

```bash
./dswfpga [reset] [chunk]
```

| Argument | Default | Meaning |
|---|---|---|
| `reset` | 0 | 1 = `libusb_reset_device()` before the sequence |
| `chunk` | 0 | 0 = one 466,408-byte transfer; otherwise chunk size |

Reads `/tmp/dswave-fpga.bin` (dump it with `DSW_FPGA_RECOVER` or the helper's
built-in bitstream capture), sends the pre-upload sequence
(`b1 f0/f2/ff`, `b0 fe fe ff`, `b0 fe 01 00`, `b1 ff`, `b0 fc e8 1d`,
`b0 fd 07 00`) and writes the bitstream to endpoint `0x02`.

Known result (2026-10-30):

| Mode | Result |
|---|---|
| whole 466,408 bytes, no reset | NAK (`LIBUSB_ERROR_TIMEOUT`) |
| whole 466,408 bytes, with reset | transfer completes, device still reports "not configured" |
| 405,504-byte chunk | `LIBUSB_ERROR_IO` |

The bitstream itself is byte-identical to the Windows capture (first 65,508
bytes compared), so the remaining gap is FX3 state, not data.
