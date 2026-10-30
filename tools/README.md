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

It resets the device, claims interface 0, **replays the application's full
bring-up configuration** (`dswcfg.h`: 1,202 captured control writes from one
real DSWave session, transcribed from the helper's log), then runs the
stop→start recovery sequence and reads until an error or the time limit.
The FPGA must already be configured — `dswbulk` does not upload the
bitstream (that is `dswfpga`'s job).

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
./dswfpga [reset] [chunk] [attempts]
```

Reads `/tmp/dswave-fpga.bin` (the helper automatically dumps the first large
`0x02` OUT it sees to that file), then sends the pre-upload sequence
(`b1 f0/f2/ff`, `b0 fe fe ff`, `b0 fe 01 00`, `b1 ff`, `b0 fc e8 1d`,
`b0 fd 07 00`) with **15 ms between commands**, waits until `b2` reports
`03 00` (idle) before PROG, writes the bitstream to endpoint `0x02` and then
polls `b2` for the `0x80` (configured) bit. Exit code 0 = configured.

| Argument | Default | Meaning |
|---|---|---|
| `reset` | 0 | 1 = `libusb_reset_device()` before the sequence |
| `chunk` | 0 | 0 = one 466,408-byte transfer; otherwise chunk size |
| `attempts` | 3 | retries of the whole sequence |

Results (2026-10-30):

| Mode | Result |
|---|---|
| v1: no delays, no idle wait, trailing `b1 f0` | transfer ok, `b2` stays `03 00` |
| v1 + `reset=1` | transfer ok, still `03 00` |
| 405,504-byte chunk | `LIBUSB_ERROR_IO` |
| **v3: idle-wait + 15 ms spacing, fresh replug** | **`b2 → 83 00`, first attempt** |

So the FPGA can be configured **without any Windows** now. Note: after a
native configure the acquisition engine still needs the application's
post-configure initialization (SCPI/calibration), which DSWave performs
itself — `dswbulk` alone will read nothing until that has run.
