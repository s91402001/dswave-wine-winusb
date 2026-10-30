# dswave-wine-winusb

Run the **Windows DSWave** application for the DreamSourceLab **DSCope M454**
(`2a0e:0080`) on Linux, using Wine plus a user-mode WinUSB shim.

**English** | [繁體中文](README.zh-TW.md)

> **Status (2026-10-30)** — device enumeration and capture work; a 30-second
> capture runs cleanly. `Stop` reliability is still being tested, and the FPGA
> bitstream upload does not work under Wine yet (a one-time configure from a
> Windows VM is required after every power cycle).

---

## What works, what doesn't

| Area | State | Notes |
|---|---|---|
| Device enumeration | ✅ | Synthetic PnP tree + hub IOCTLs |
| `H0: DSCope M454` tab, READY | ✅ | `Fpga is already done` when the FPGA is configured |
| Capture / waveform rendering | ✅ | 30+ seconds, zero `-EPROTO` after the fixes |
| `Stop` → `Run` again | ⚠️ | Under test; see `docs/progress.md` |
| FPGA bitstream upload | ❌ | Transfer completes but the FPGA does not configure (see Roadmap) |
| Replug without Windows | ❌ | Requires a Windows VM to configure the FPGA once |

---

## Why this exists

* Wine's `winusb.dll` is a stub, and `wineusb.sys` is only a bus driver: there
  is no WinUSB function driver and no user-mode device interface, so Windows
  applications see no WinUSB devices at all.
* DSWave loads `C:\windows\system32\{SetupAPI,WinUSB}.dll` **by absolute path**
  and resolves the API with `GetProcAddress`, so putting replacement DLLs next
  to the executable does not work — they must override `system32`.

---

## Architecture

```
DSWave.exe (libusb-win32 / libusb statically linked)
  |
  +-- SetupDi* / CM_*  -->  setupapi.dll + cfgmgr32.dll shims
  |                         synthetic PnP tree:
  |                           HCD 0x3000 -> HUB 0x2000 -> DEV 0x1000
  |                         device interface path = a real empty file
  |                         (CreateFile succeeds, no kernel driver involved)
  |
  +-- WinUsb_*          -->  winusb.dll shim (20 entry points + helpers)
                             |
                             |  TCP 127.0.0.1:47123 (length-prefixed RPC)
                             v
                        helper (native Linux, libusb-1.0)
                             |
                             v
                        /dev/bus/usb/BBB/DDD  (real DSCope M454)
```

The Windows side also hooks three imports inside `DSWave.exe`:

| Import | Why |
|---|---|
| `DeviceIoControl` | answer hub IOCTLs (`GET_NODE_CONNECTION_INFORMATION_EX`, `GET_DESCRIPTOR_FROM_NODE_CONNECTION`, ...) |
| `CreateIoCompletionPort` | remember the IOCP so transfers can post completions |
| `CancelIoEx` | complete a pending transfer as cancelled (libusb's cancel path would otherwise silently fail on a plain file) |

See [`docs/architecture.md`](docs/architecture.md) for the RPC protocol, the
bulk-transfer lifecycle and the status-code mapping.

---

## Requirements

* Linux (x86_64), Wine (tested with the `devel` branch; Wine 11.x)
* `libusb-1.0` development files
* `mingw-w64` cross compiler (`x86_64-w64-mingw32-gcc`)
* Python 3 (used by `scripts/gen_def.py`)
* A Wine prefix with **DSWave v1.1.0** installed
* The device, already FPGA-configured (see the status table)

---

## Build

```bash
cd wine-winusb-shim
./scripts/build.sh
```

`WINE_LIB` defaults to `/opt/wine-devel/lib/wine/x86_64-windows`; override it
if your Wine lives elsewhere:

```bash
WINE_LIB=/usr/lib/wine/x86_64-windows ./scripts/build.sh
```

Outputs in `wine-winusb-shim/build/`:

| File | Meaning |
|---|---|
| `setupapi.dll` | enumeration + hub IOCTL shim (forwards the other 597 exports) |
| `cfgmgr32.dll` | `CM_Get_Parent` / `CM_Get_Child` shim |
| `winusb.dll` | WinUSB API shim (must be linked with `-Wl,--export-all-symbols`) |
| `helper` | native Linux libusb RPC daemon |

## Install

```bash
cd wine-winusb-shim
WINEPREFIX=~/.wine-dswave ./scripts/install.sh
```

The script copies the DLLs into `system32`, sets the `native` DLL overrides,
backs up Wine's originals and starts the helper in the background.

## Run

```bash
# helper (started automatically by install.sh; restart manually after a rebuild)
cd wine-winusb-shim
DSW_DEBUG=1 DSW_MBPS=20 setsid nohup ./build/helper >/tmp/dswave-helper.log 2>&1 &

# the application
WINEPREFIX=~/.wine-dswave wine "C:\\Program Files\\DSWave\\DSWave.exe" -l 5
```

Uninstall: `WINEPREFIX=~/.wine-dswave ./scripts/uninstall.sh`.

---

## Environment variables

| Variable | Default | Meaning |
|---|---|---|
| `DSW_DEBUG` | `0` | verbose helper + libusb logging |
| `DSW_MBPS` | `0` | read-rate cap in MB/s (`20` matches the Windows baseline) |
| `DSW_CHUNK_BYTES` | `405504` | bulk IN transfer size |
| `DSW_FILL` | `1` | chain device packets until the caller's buffer is full |
| `DSW_NO_RESET` | `0` | skip `libusb_reset_device()` on first open |
| `DSW_LIST_LIVE` | `0` | disable the descriptor cache (debug only) |
| `DSW_SOFT_RESET` | `0` | replay the stop sequence when the device is closed |
| `DSW_FPGA_RECOVER` | `0` | reset + replay the pre-upload sequence before a bitstream upload |
| `DSW_CANCELIO_HOOK` | `1` | enable the `CancelIoEx` hook |

---

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `LIBUSB_ERROR_ACCESS` when opening | `winusb.dll` lost its exports — rebuild with `-Wl,--export-all-symbols` and check `objdump -p build/winusb.dll \| grep WinUsb_` |
| `LIBUSB_ERROR_BUSY` (`claim interface`) | `virt-manager`/QEMU still holds the device; detach the USB redirection |
| READY, but `Run` shows nothing | device is wedged: reconfigure the FPGA from the Windows VM, or replug |
| UI freezes on `Stop` | enable `DSW_DEBUG=1`; inspect `cancelio.log` and the helper log |
| Waveform stops after ~1 s | check for `-EPROTO` (urb status `-71`) in the helper log |

Logs:

* `/tmp/dswave-helper.log`
* `${WINEPREFIX}/drive_c/ProgramData/DSWaveShim/winusb.log`
* `${WINEPREFIX}/drive_c/ProgramData/DSWaveShim/setupapi.log`
* `${WINEPREFIX}/drive_c/ProgramData/DSWaveShim/cancelio.log`

---

## Tested versions

These binaries are **not** part of the repository; only their versions are recorded.

| File | Size (bytes) | Purpose |
|---|---|---|
| `DSWave_v1.1.0_x64_setup.exe` | 48,637,552 | vendor installer (v1.1.0, x64) |
| `output.pcap` | 129,023,508 | Windows USBPcap baseline (normal capture) |
| `fpga1.pcap` | 8,920,535 | Windows USBPcap baseline (FPGA configure + run) |

The two captures are included **gzip-compressed** under
[`usbpcap/data/`](usbpcap/data/README.md) (2.0 MB and 29.8 MB); the installer is
not redistributed.

* Device: `2a0e:0080`, USB 3.0 SuperSpeed (5 Gb/s).
* Wine: `devel` build, 11.x.
* Baseline transport on Windows: 405,504-byte bulk IN transfers, ~22 ms apart
  (≈18.6 MB/s).

Capture instructions: [`usbpcap/README.md`](usbpcap/README.md).

---

## Development notes

* **Never poll EP0 during a bulk stream.** The app's hot-plug thread enumerates
  every ~100 ms; each enumeration used to open the device and issue
  `GET_DESCRIPTOR`, which froze both EP0 and the 0x86 bulk stream. Descriptors
  are now cached per `bus:addr`.
* **Never slice bulk URBs with short timeouts.** Re-submitting a cancelled URB
  produces `-EPROTO` (`urb status -71`). Transfers are submitted asynchronously
  with the caller's full timeout.
* **`OVERLAPPED.Internal` must be an NTSTATUS** (`STATUS_CANCELLED = 0xC0000120`),
  not a `USBD_STATUS` (`0xC0010000`).
* **The FPGA loader needs the whole bitstream in one transfer.** Splitting
  466,408 bytes into 405,504 + 60,904 fails with `LIBUSB_ERROR_IO`.

---

## Repository layout

```
docs/                 progress, plan, architecture, analysis notes
tools/                standalone diagnostics (dswbulk, dswfpga)
wine-winusb-shim/     the shim itself (src/ + scripts/)
usbpcap/              capture & parse helpers (Windows / Linux)
```

## Roadmap

1. Finish the `Stop` → `Run` fix and make it deterministic.
2. Make the FPGA bitstream upload work under Wine (single-transfer upload is
   already verified; the device still reports "not configured").
3. Notify the app (`WM_DEVICECHANGE`) when the device re-enumerates.
4. Optional: automated capture smoke test.

## License

MIT — see [LICENSE](LICENSE).

## Disclaimer

This project is an interoperability effort: it implements a Wine-side shim for
an application we own/run. It contains **no vendor binaries**. DSWave,
DreamSourceLab and DSCope are trademarks of their respective owners.
