# Architecture

How the Windows DSWave process is connected to a Linux `libusb-1.0` device.

---

## 1. Why a shim at all

Wine provides `winusb.dll` as a stub and `wineusb.sys` as a bus driver only.
There is no WinUSB function driver, no device interface class, and therefore no
handle that an application can open. On top of that, DSWave loads
`C:\windows\system32\{setupapi,winusb}.dll` by absolute path and resolves
symbols with `GetProcAddress`, so local DLLs next to the executable are ignored.

The shim therefore does two things:

1. **`setupapi.dll` / `cfgmgr32.dll`** — pretend a WinUSB device exists by
   synthesising a PnP tree and answering the enumeration calls.
2. **`winusb.dll`** — implement the WinUSB API on top of a native Linux helper.

---

## 2. Synthetic PnP tree

```
DEVINST 0x3000  PCI\VEN_8086&DEV_1E31...      "USB xHCI Compliant Host Controller"
   |
DEVINST 0x2000  USB\ROOT_HUB30\4&0&0          "USB Root Hub (USB 3.0)"
   |
DEVINST 0x1000  USB\VID_2A0E&PID_0080\<bus>&<addr>   "USB-based DSL Instrument v3"
```

* The device interface path is a real, empty file:

  ```
  \\?\C:\ProgramData\DSWaveShim\USB#VID_2A0E&PID_0080#<bus>&<addr>#{A5DCBF10-6530-11D2-901F-00C04FB951ED}
  ```

  `CreateFile()` on it succeeds, so libusb's WinUSB backend gets a handle; the
  handle is then interpreted by `winusb.dll` (`parse_device_path()` extracts
  `bus`/`addr` from the file name).

* `SetupDiGetClassDevs` / `EnumDeviceInterfaces` / `GetDeviceInterfaceDetail`
  return this synthetic node. Everything else (597 exports) is forwarded to
  Wine's original `setupapi_orig.dll` through a generated `.def` file.

* Hub IOCTLs are answered from the descriptors supplied by the helper
  (`REQ_LIST`), so libusb can populate its device list without opening the
  device. **This is cached**: the app enumerates every ~100 ms and each
  uncached enumeration used to issue EP0 `GET_DESCRIPTOR`, which freezes the
  FX3 in the middle of a bulk stream.

---

## 3. RPC protocol

`src/proto.h`, TCP `127.0.0.1:47123`, one connection per thread that needs to
talk to the helper.

```c
struct shim_req {          /* Windows -> helper */
    uint32_t type;         /* REQ_LIST/OPEN/CLOSE/CONTROL/BULK/DESCRIPTOR/... */
    uint32_t handle;
    uint32_t a, b, c, d;   /* request-specific */
    uint32_t len;          /* payload length */
};

struct shim_resp {         /* helper -> Windows */
    int32_t  status;       /* 0 or negative libusb error */
    uint32_t len;
    uint32_t handle;
    uint32_t bus, addr, vid, pid, speed, interfaces;
};
```

| Type | Meaning |
|---|---|
| `REQ_LIST` (1) | enumerate `2a0e:0080`, return `struct dev_info` (descriptors included) |
| `REQ_OPEN` (2) | `libusb_open` + `claim_interface(0)` |
| `REQ_CLOSE` (3) | release + close |
| `REQ_CONTROL` (4) | control transfer |
| `REQ_BULK` (5) | bulk IN/OUT; `a` = endpoint, `b` = timeout ms, `d` = sequence |
| `REQ_DESCRIPTOR` (6) | raw descriptor read |
| `REQ_ABORT_PIPE` (7) | bump the abort generation for one endpoint |
| `REQ_RESET_PIPE` (8) | `libusb_clear_halt` |

Bulk IN payloads are streamed after the response header. The helper serialises
requests per connection (`serve()` reads one request, executes, writes one
response), which is why the Windows side uses a small pool of worker sockets
instead of one shared connection.

---

## 4. Bulk transfer lifecycle (Windows side)

```
WinUsb_ReadPipe/WritePipe (overlapped)
   -> submit_async(): calloc bulk_ctx, add to active_list, push to queue
   -> bulk_worker thread: rpc_on(worker_sock[i], REQ_BULK, payload)
   -> finish_bulk(): unlink, set OVERLAPPED.Internal (NTSTATUS),
                     InternalHigh (bytes), PostQueuedCompletionStatus()
```

* `OVERLAPPED.Internal` must be a **NTSTATUS**. libusb's IOCP path calls
  `GetOverlappedResult()` and Wine maps `RtlNtStatusToDosError()`, so
  `STATUS_CANCELLED (0xC0000120) → ERROR_OPERATION_ABORTED (995) →
  LIBUSB_TRANSFER_CANCELLED`. A `USBD_STATUS` such as `0xC0010000` is not a
  valid NTSTATUS and is misreported as "completed with 0 bytes".
* An `OVERLAPPED` is completed exactly once: `bulk_ctx.completed` is set under
  `active_lock` before posting, so a late worker cannot double-post.

### Cancellation

`CancelIoEx` is hooked. On call it first tries `shim_cancel_overlapped(ov)`
(exported by `winusb.dll`), which:

1. finds the `bulk_ctx` whose `overlapped` matches,
2. posts `STATUS_CANCELLED` immediately,
3. sends `REQ_ABORT_PIPE` so the helper unwinds the USB transfer.

Only if the `OVERLAPPED` is not ours does it fall back to Wine's `CancelIoEx`
(or returns `ERROR_NOT_FOUND` for our shim files, matching Windows semantics).

---

## 5. Bulk transfer lifecycle (helper side)

`do_bulk()`:

```
snapshot abort_gen[handle][ep]        <- BEFORE taking the lock
lock ep_lock[handle][ep]
if abort_gen changed while queued -> return -10 immediately
loop over chunks:
    libusb_submit_transfer(chunk, timeout = caller's timeout)
    while (!done)
        if abort_gen changed and not cancelled: libusb_cancel_transfer() once
        libusb_handle_events_timeout_completed(50 ms)
    wait for the callback before freeing (no use-after-free)
    deliver partial + status
```

* Chunks are 405,504 bytes for IN (matching the Windows URB size).
* **OUT transfers are never split** — the FPGA loader requires the entire
  466,408-byte bitstream in one transfer.
* There is no 200 ms URB slicing: a URB stays queued until data arrives, the
  caller's timeout expires, or an abort is requested. Re-submitting cancelled
  URBs was the source of `-EPROTO`.

### Control commands that matter

| Command (`b0` data) | Meaning | Aborts pending reads? |
|---|---|---|
| `01 00 00` | stop acquisition | no (also used for reconfiguration) |
| `02 08 00` | flow pause | no (mid-capture settings changes use it) |
| `02 04 00` | FIFO / hardware reset | yes |
| `fc e8 1d`, `fd 07 00` | FPGA PROG / init | — |

The app's stop sequence is `01 00 00 → 02 08 00 → read 0x84 ×3 → 01 00 00 →
02 04 00`, but it blocks until pending `0x86` reads complete before sending
`02 04 00`. Cancellation must therefore come from `CancelIoEx`, not from the
control commands.

---

## 6. Status mapping

| libusb | `OVERLAPPED.Internal` | Wine `GetLastError` | libusb status seen by the app |
|---|---|---|---|
| `-7` timeout | `STATUS_IO_TIMEOUT` | `ERROR_SEM_TIMEOUT` | `LIBUSB_TRANSFER_TIMED_OUT` |
| `-10` interrupted | `STATUS_CANCELLED` | `ERROR_OPERATION_ABORTED` | `LIBUSB_TRANSFER_CANCELLED` |
| `-1` I/O (EPROTO) | `STATUS_DEVICE_DATA_ERROR` | `ERROR_CRC` | `LIBUSB_TRANSFER_ERROR` |
| other | `STATUS_UNSUCCESSFUL` | `ERROR_GEN_FAILURE` | `LIBUSB_TRANSFER_STALL` |

---

## 7. Known limitations

* The FPGA bitstream upload completes at the USB level but the device still
  reports "not configured"; a Windows VM is required after each power cycle.
* Device re-enumeration is not announced to the app with `WM_DEVICECHANGE`,
  so a stale handle can survive until the app rescans.
* The helper is a separate process; restarting it closes the device.
