# 針對 `progress.md`（2026-10-30 進度）之根因分析與改進建議 (`argon_reply8.md`)

檢視完 [`progress.md`](../progress.md#L595-L669) 最新進度、[`helper.c`](../../wine-winusb-shim/src/helper.c)、[`winusb_shim.c`](../../wine-winusb-shim/src/winusb_shim.c)、[`setupapi_shim.c`](../../wine-winusb-shim/src/setupapi_shim.c) 以及實際執行日誌（[`/tmp/dswave-helper.log`](file:///tmp/dswave-helper.log) 與 [`winusb.log`]($WINEPREFIX/drive_c/ProgramData/DSWaveShim/winusb.log)）後，首先要肯定 `/tmp/dswbulk` 獨立驗證的突破性價值：

- **`/tmp/dswbulk` 連續 5 秒讀取 170 MB（34 幀、20 MB/s、0 錯誤）**，已經徹底排除「Linux xHCI 驅動不支援此硬體」或「SCPI 初始化序列不完整」的假設。
- 目前剩下的三個核心問題（[**問題 1**：Wine 下 0.3–1.1 秒即發生 `-EPROTO` (`-71`)](../progress.md#L649)、[**問題 2**：`0xC0010000` 被 App 視為 `LIBUSB_TRANSFER_COMPLETED; length: 0`](../progress.md#L627)、[**問題 3**：STOP 或 URB 取消後裝置卡在「僅 4096B 標頭、無幀資料」](../progress.md#L654-L656)），在日誌與程式碼中都有明確的直接根因（Smoking Gun）。

以下按優先順序說明根因與具體修改方案。

---

## 一、頭號真兇（Smoking Gun）：為什麼 `/tmp/dswbulk` 跑 5 秒 0 錯誤，但在 Wine 下 0.3–1.1 秒就 `-EPROTO`？

### 1. 日誌證據：`DSWave.exe` 背景熱插拔執行緒每 100 ms 觸發 `REQ_LIST`，在 `0x86` 高速傳輸期間每秒對裝置執行 20 次 `libusb_open` + 60 次 EP0 `GET_DESCRIPTOR` + 20 次 `libusb_close`！

請看 [`/tmp/dswave-helper.log`](file:///tmp/dswave-helper.log#L130399-L130721) 在 `seq=1`（`87.590s` 開始讀取 `0x86`）到 `87.825s` 發生 `-EPROTO` 之間不到 **235 ms** 內發生了什麼事：

- `87.601s`、`87.702s`、`87.704s`、`87.806s`、`87.808s`：`DSWave.exe` 內建的熱插拔偵測執行緒每 **100 ms** 呼叫一次 `libusb_get_device_list()`。
- 每次呼叫都會觸發兩次 [`SetupDiGetClassDevsW`](../../wine-winusb-shim/src/setupapi_shim.c#L305-L322)（[`make_set(0)` 與 `make_set(1)`](../../wine-winusb-shim/src/setupapi_shim.c#L270-L295)），而 [`make_set`](../../wine-winusb-shim/src/setupapi_shim.c#L283) 每次都無條件呼叫 [`refresh_dev_identity()`](../../wine-winusb-shim/src/setupapi_shim.c#L179-L212) -> [`shim_get_dev_info()`](../../wine-winusb-shim/src/ioctl_hook.c#L82-L120)，向 `helper` 發送 `REQ_LIST`（`req=1`）。
- 在 [`helper.c`](../../wine-winusb-shim/src/helper.c#L231-L263) 中，[`do_list()`](../../wine-winusb-shim/src/helper.c#L265-L305) 呼叫 [`read_descriptors()`](../../wine-winusb-shim/src/helper.c#L235-L263)，雖然註解寫著 *"this is cheap, does not disturb a running capture"*，但實際上它每次都會：
  1. 呼叫 [`libusb_open(dev, &h)`](../../wine-winusb-shim/src/helper.c#L241) 開啟新的 `/dev/bus/usb` fd；
  2. 對 **EP0 發送 3 筆真實的匯流排 Control Transfer (`GET_DESCRIPTOR`)**（[`libusb_get_descriptor`](../../wine-winusb-shim/src/helper.c#L244) 18B + [`libusb_control_transfer`](../../wine-winusb-shim/src/helper.c#L250-L257) 9B 與 57B）；
  3. 呼叫 [`libusb_close(h)`](../../wine-winusb-shim/src/helper.c#L261)，這會在同一個 `libusb_context` 內觸發 `someone is closing a device`（見 [`/tmp/dswave-helper.log` L130414-L130419](file:///tmp/dswave-helper.log#L130414-L130419)、[L130485](file:///tmp/dswave-helper.log#L130485)、[L130555](file:///tmp/dswave-helper.log#L130555)、[L130625](file:///tmp/dswave-helper.log#L130625)、[L130695](file:///tmp/dswave-helper.log#L130695)），強制打斷正在執行 `0x86` Bulk IN 的背景執行緒 `[002521e5]` 的 `poll()` 事件迴圈！

這完美解釋了為什麼 `/tmp/dswbulk` 跑 5 秒完全不會 `-EPROTO`，而 Wine 下跑 0.3–1.1 秒必死：
- `/tmp/dswbulk` 沒有背景執行緒每 50–100 ms 開關裝置並轟炸 FX3 的 EP0；
- FX3 在處理 `0x86` SuperSpeed GPIF-II DMA 高速串流時，若每秒被 60 筆 EP0 `GET_DESCRIPTOR` 控制傳輸與 `libusb_close` 打斷，FX3 韌體 / DMA 狀態機極易發生微秒級的匯流排協定超時（`-EPROTO` / `-71`）。

### 2. 具體修改方案（徹底消除 `REQ_LIST` 的匯流排與 `libusb_open/close` 開銷）

1. **修改 [`helper.c`](../../wine-winusb-shim/src/helper.c#L235-L263) 的 [`read_descriptors()`](../../wine-winusb-shim/src/helper.c#L235-L263)：完全不要呼叫 `libusb_open()` 或發送 USB 封包！**
   - Linux 核心在枚舉 USB 裝置時，早已將完整的原始二進位 Device Descriptor（18B）與 Configuration Descriptor（57B）快取在記憶體中：
     - 方法 A（最簡單，純 `libusb` 無須 `open`）：在 [`helper.c`](../../wine-winusb-shim/src/helper.c#L235-L263) 中，直接透過 `/sys/bus/usb/devices/<bus>-<port>/descriptors` 讀取核心快取檔案，**或**只要 `(bus, addr, vid, pid)` 與上次相同，就直接回傳靜態快取 `static struct dev_info cached_dev_info`（僅在第一次見到該 `bus:addr` 且 `handles[1] == NULL` 時讀一次）。
     - 方法 B：若 `handles[1] != NULL`（裝置已開啟），[`do_list()`](../../wine-winusb-shim/src/helper.c#L265-L305) **絕對不要**再呼叫 `libusb_get_device_list()` 或 `read_descriptors()`，直接回傳 [`do_open()`](../../wine-winusb-shim/src/helper.c#L307-L388) 時快取下來的 `cached_dev_info`！
2. **修改 [`setupapi_shim.c`](../../wine-winusb-shim/src/setupapi_shim.c#L179-L212) 的 [`refresh_dev_identity()`](../../wine-winusb-shim/src/setupapi_shim.c#L179-L212)：加上時間節流（Throttle）**
   - 在 [`refresh_dev_identity()`](../../wine-winusb-shim/src/setupapi_shim.c#L179-L212) 記錄 `GetTickCount()`，若距離上次成功取得 `cur_dev` 未滿 `2000 ms`（2 秒），直接沿用快取的 `cur_dev`，連 TCP socket IPC (`REQ_LIST`) 都不用發。

---

## 二、問題 2 根因：為什麼 `0xC0010000` 被 `DSWave.exe` 判定為 `LIBUSB_SUCCESS / LIBUSB_TRANSFER_COMPLETED; length: 0`？

[`progress.md`](../progress.md#L627) 與 [L651-L653](../progress.md#L651-L653) 疑惑為何將 `ov->Internal` 設為 `0xC0010000` (`USBD_STATUS_CANCELED`) 後，App 卻印出：
```text
app: Data transfer failed: LIBUSB_SUCCESS / LIBUSB_TRANSFER_COMPLETED; length: 0
```

### 1. 根因剖析（[`winusb_shim.c`](../../wine-winusb-shim/src/winusb_shim.c) 的三個狀態碼錯誤）

1. **為什麼 App 印出 `LIBUSB_SUCCESS / LIBUSB_TRANSFER_COMPLETED`？**
   - 在 `libusb` 中，`LIBUSB_TRANSFER_COMPLETED` 的數值就是 `0`（與 `LIBUSB_SUCCESS == 0` 相同）。當 `DSWave.exe` 的 `TransferThread` 發現 `transfer->status != LIBUSB_TRANSFER_COMPLETED || transfer->actual_length == 0` 時，它呼叫 `libusb_error_name(transfer->status)` 印出錯誤訊息；因為 `transfer->status == 0` 且 `actual_length == 0`，`libusb_error_name(0)` 就會印出 `"LIBUSB_SUCCESS / LIBUSB_TRANSFER_COMPLETED"`！
2. **為什麼 `transfer->status` 會變成 `0` (`LIBUSB_TRANSFER_COMPLETED`)？**
   - 檢視 `DSWave.exe` 的匯入表與符號表，其內嵌的 `libusb` 使用 **`windows_iocp_thread`**（`GetQueuedCompletionStatus` -> `windows_handle_transfer_completion` -> `GetOverlappedResult` -> `winusbx_copy_transfer_data`）。
   - 請看 [`winusb_shim.c`](../../wine-winusb-shim/src/winusb_shim.c#L312-L330) 的 [`finish_bulk()`](../../wine-winusb-shim/src/winusb_shim.c#L312-L330)：
     ```c
     static void finish_bulk(struct bulk_ctx *b, ULONG status, ULONG actual)
     {
         ...
         ov->Internal = status;
         ov->InternalHigh = actual;
         if (b->dev && b->dev->iocp)
             PostQueuedCompletionStatus(b->dev->iocp, (DWORD)actual, b->dev->iocp_key, ov);
     ```
   - **致命陷阱 A（`PostQueuedCompletionStatus` 覆寫與 Wine `GetOverlappedResult` 行為）**：
     在 Windows / Wine 的 `KERNEL32` 中，`OVERLAPPED.Internal` 存放的是 **`NTSTATUS`**（例如 `STATUS_SUCCESS = 0`、`STATUS_CANCELLED = 0xC0000120`、`STATUS_UNSUCCESSFUL = 0xC0000001`），**不是** `USBD_STATUS` (`0xC0010000`)！
     更嚴重的是，當 `libusb` 在 `windows_handle_transfer_completion` 中呼叫 `KERNEL32!GetOverlappedResult(hFile, ov, &bytes, FALSE)`（或 [`WinUsb_GetOverlappedResult`](../../wine-winusb-shim/src/winusb_shim.c#L978-L1014)）時：
     - 在 Wine 的 `GetOverlappedResult` 中，它將 `ov->Internal` 傳給 `RtlNtStatusToDosError(ov->Internal)`。而 **`0xC0010000` 不是合法的 `NTSTATUS`**（Customer bit 未設，Facility `0x0001` 在 `ntdll` 中無對應表）！
     - 而在 [`WinUsb_GetOverlappedResult`](../../wine-winusb-shim/src/winusb_shim.c#L1000-L1013) 中，`switch (overlapped->Internal)` 只處理了 `STATUS_SUCCESS`、`STATUS_IO_TIMEOUT`、`STATUS_CANCELLED` (`0xC0000120`)，沒有處理 `0xC0010000`！
   - **致命陷阱 B（64-bit `ULONG` 零擴展與 `winusbx_copy_transfer_data`）**：
     在 64-bit (`x86_64`) 下，`ov->Internal` 的型別是 `ULONG_PTR`（64-bit）。而 `libusb` 的 `winusbx_copy_transfer_data` 檢查傳輸是否成功時，是看 `GetOverlappedResult` 的回傳值與 `GetLastError()`（或檢查 `(NTSTATUS)ov->Internal`）。若 `GetOverlappedResult` 因為 `PostQueuedCompletionStatus`（在 Wine `NtSetIoCompletion` 中傳入的 `IoStatus.Status` 永遠是 `STATUS_SUCCESS = 0`！）或因為未觸發 `ERROR_OPERATION_ABORTED (995)`，就會導致 `winusbx_copy_transfer_data` 走進成功分支，回傳 `LIBUSB_TRANSFER_COMPLETED` 且 `length = 0`！
   - **致命陷阱 C（在 [`run_bulk_worker`](../../wine-winusb-shim/src/winusb_shim.c#L394-L402) 把硬體錯誤 `-EPROTO` 偽裝成 `CANCELLED`）**：
     請看 [`winusb_shim.c` L394-L402](../../wine-winusb-shim/src/winusb_shim.c#L394-L402)：
     ```c
     else if (b->is_in && resp.status == -1 /* LIBUSB_ERROR_IO, e.g. EPROTO */)
     {
         shim_log("worker post seq=%u bytes=%u status=CANCELLED(io)", b->seq, resp.len);
         finish_bulk(b, 0xC0010000 /* USBD_STATUS_CANCELED */, 0);
     }
     ```
     當 `seq=1` 發生 `-EPROTO` 時，App **根本還沒呼叫 `libusb_cancel_transfer`**！此時該 transfer 在 `libusb` 內部並沒有 `LIBUSB_TRANSFER_CANCELLED` 旗標。

### 2. 具體修改方案

1. **在 [`winusb_shim.c`](../../wine-winusb-shim/src/winusb_shim.c#L312-L330) 中統一使用標準 `NTSTATUS`**：
   - 當傳輸被 `WinUsb_AbortPipe` / `REQ_ABORT_PIPE` 取消（`resp.status == -10` 即 `LIBUSB_ERROR_INTERRUPTED`）時，傳入 **`STATUS_CANCELLED` (`0xC0000120`)**，使 `RtlNtStatusToDosError(0xC0000120)` 與 [`WinUsb_GetOverlappedResult`](../../wine-winusb-shim/src/winusb_shim.c#L1007-L1009) 都正確回傳 `FALSE` 並設定 `SetLastError(ERROR_OPERATION_ABORTED)` (`995`)。
   - 當傳輸發生真實 I/O 錯誤（`resp.status == -1`）且**不是**因為 `WinUsb_AbortPipe` 時：若 `resp.len > 0`（例如已收到 335,872 bytes 後因 STOP 中斷），且此時 `abort_pipe` 已被觸發，才回報 `STATUS_CANCELLED` (`0xC0000120`)；否則回報 `STATUS_DEVICE_DATA_ERROR` (`0xC000009C`) 或 `STATUS_UNSUCCESSFUL` (`0xC0000001`)。

---

## 三、問題 3 根因：為什麼 STOP 或 URB 取消後，裝置會卡死在「僅回傳 4096B 標頭、無幀資料」？

[`progress.md` L638](../progress.md#L638) 與 [L654-L656](../progress.md#L654-L656) 觀察到一個極為關鍵的現象：
> 即使在純 Linux 的 `/tmp/dswbulk` 中，只要發生任何 STOP（`01 00 00`）或主機端 URB 取消（`libusb_cancel_transfer`），裝置就會卡死，下次只能讀到 4096B 標頭，再也讀不到 `0x86` 幀資料。

### 1. 為什麼會發生？兩個層面的原因

1. **原因 A：`helper.c` 的 `200 ms` 切片超時（`DSW_SLICE_MS = 200`）在正常擷取中主動取消了 URB！**
   - 請看 [`helper.c` L755-L845](../../wine-winusb-shim/src/helper.c#L755-L845)：[`do_bulk()`](../../wine-winusb-shim/src/helper.c#L728-L896) 預設將 `300,000 ms` 的傳輸切成每 **`200 ms`** 一次的 `libusb_bulk_transfer(..., slice)`！
   - 這意味著什麼？只要 FPGA 因為等待觸發（Trigger）、慢速時基（Slow timebase）或幀間空檔超過 **200 ms** 沒有送出資料，`libusb_bulk_transfer` 就會因為 200 ms 超時而在 Linux xHCI 驅動中**執行 `USBDEVFS_DISCARDURB`（取消正在飛行中的 SuperSpeed Bulk IN URB）**，然後立刻重新提交下一個 URB！
   - 對於 `bMaxBurst = 3` 的 Cypress FX3 SuperSpeed 端點 `0x86`，當 FX3 / FPGA GPIF-II 正準備推送 Burst 封包時，若主機端 xHCI 每 200 ms 不斷 Discard URB 又 Resubmit URB，就會直接造成 FX3 DMA 通道與 xHCI Endpoint Ring 失步（立即引發 `-EPROTO` 或卡死 FPGA FIFO）！
   - `/tmp/dswbulk` 之所以沒壞，是因為 `/tmp/dswbulk` 使用較長的超時（如 `2000 ms` 或直接連續讀取），在 20 MB/s 下沒有觸發 200 ms 的 URB Discard！

2. **原因 B：[`do_control()`](../../wine-winusb-shim/src/helper.c#L548-L560) 在收到第一筆 `01 00 00` 時「提早自動取消」了 `0x86`，破壞了硬體的完整 STOP 握手序列！**
   - 請對照 [`progress.md` L615](../progress.md#L615) 的 Windows USBPcap 正常 STOP 序列：
     1. `01 00 00`（停止觸發/擷取狀態機）
     2. `02 08 00`（切換/鎖定 FIFO 狀態）
     3. 從 `0x84` 讀取 1 byte 狀態 × 3 次（確認 FPGA DMA 已排空停止）
     4. `01 00 00`
     5. **`02 04 00`（重置 FPGA 擷取 FIFO / 清除殘留資料）**
   - 再看 [`helper.c` L548-L560](../../wine-winusb-shim/src/helper.c#L548-L560)：
     當 `DSWave.exe` 剛送出第 1 步 `01 00 00` 時，[`do_control()`](../../wine-winusb-shim/src/helper.c#L548-L560) 就**擅自把 `abort_gen` + 1，強制取消 `0x86` 的 URB**！
   - 這導致兩個嚴重後果：
     - 第一，當 FPGA 還沒收到 `02 08 00` 與 `02 04 00`、GPIF-II 還在往 FX3 塞資料時，主機端突然 Discard URB，導致 FX3 端點 Buffer 卡住殘留資料；
     - 第二，當 `-EPROTO` 發生時，`DSWave.exe` 走進錯誤處理分支（印出 `usb device is not ready: ERROR`），**跳過了後續的 `02 04 00` FIFO 重置指令**！於是下次啟動時，FX3 只送得出它自己 RAM 裡組裝的 4096-byte 標頭，而 FPGA 端的 GPIF FIFO 因為從未收到完整的 `01 00 00 -> 02 08 00 -> 0x84*3 -> 01 00 00 -> 02 04 00` 排空與重置序列而永遠卡死！

### 2. 具體修改方案（防止與自動修復「僅 4096B 標頭」卡死狀態）

1. **廢除 `200 ms` 切片取消（`DSW_SLICE_MS`），改用非同步 `libusb_submit_transfer` 等待完整 `timeout_ms`**：
   - 在 [`helper.c`](../../wine-winusb-shim/src/helper.c#L728-L896) 的 [`do_bulk()`](../../wine-winusb-shim/src/helper.c#L728-L896) 中，**絕對不要**用 `200 ms` 的 `libusb_bulk_transfer` 迴圈去模擬可取消的傳輸（因為每次 200 ms 超時都是一次核心層級的 URB Discard！）。
   - 正確做法：在 [`do_bulk()`](../../wine-winusb-shim/src/helper.c#L728-L896) 中直接提交一個 timeout 為 `req->d`（例如 `300000 ms`）的非同步 `struct libusb_transfer`，並在迴圈中呼叫 `libusb_handle_events_timeout_completed(ctx, &tv_50ms, &completed)` 同時檢查 `abort_gen`。
   - 如此一來，**只要沒有收到 `REQ_ABORT_PIPE`，URB 在整個 300 秒內都靜靜地掛在 xHCI 佇列中等待硬體資料，期間 0 次 URB 取消、0 次重送！** 只有當 `abort_gen` 改變時，才呼叫一次 `libusb_cancel_transfer(xfer)`。
2. **移除 [`do_control()`](../../wine-winusb-shim/src/helper.c#L548-L560) 對 `01 00 00` 的自動 `abort_gen++`**：
   - 讓 `DSWave.exe` 自己決定何時呼叫 `WinUsb_AbortPipe`（即 `REQ_ABORT_PIPE`），或者只在收到最後一步 `02 04 00` 時才自動 abort，不要在第 1 步 `01 00 00` 就腰斬正在收尾的 `0x86` 傳輸。
3. **在 [`do_open()`](../../wine-winusb-shim/src/helper.c#L367-L384) 與 [`do_close()`](../../wine-winusb-shim/src/helper.c#L510-L530) 加入硬體柔和重置序列（Soft Stop + Drain），免除重刷 FPGA**：
   - `libusb_reset_device()` 只會重置 FX3 的 USB 介面，不會重置外部 FPGA 的擷取狀態機。
   - 在 [`do_close()`](../../wine-winusb-shim/src/helper.c#L510-L530) 關閉裝置前（以及 [`do_open()`](../../wine-winusb-shim/src/helper.c#L367-L384) 開啟裝置時），主動補發完整的 Windows STOP 序列並排空殘留封包：
     ```c
     static void dsw_hw_soft_reset(libusb_device_handle *h)
     {
         unsigned char stop1[3] = {0x01, 0x00, 0x00};
         unsigned char stop2[3] = {0x02, 0x08, 0x00};
         unsigned char stop3[3] = {0x02, 0x04, 0x00};
         unsigned char st = 0;
         unsigned char *trash = malloc(405504);
         int actual = 0, i;

         libusb_control_transfer(h, 0x40, 0xb0, 0, 0, stop1, 3, 200);
         libusb_control_transfer(h, 0x40, 0xb0, 0, 0, stop2, 3, 200);
         for (i = 0; i < 3; i++)
             libusb_bulk_transfer(h, 0x84, &st, 1, &actual, 50);
         libusb_control_transfer(h, 0x40, 0xb0, 0, 0, stop1, 3, 200);
         libusb_control_transfer(h, 0x40, 0xb0, 0, 0, stop3, 3, 200);
         libusb_clear_halt(h, 0x86);
         if (trash) {
             /* Drain any residual FX3 DMA buffer so next session starts clean */
             libusb_bulk_transfer(h, 0x86, trash, 405504, &actual, 50);
             free(trash);
         }
     }
     ```
4. **若需在 Linux 下直接完成 FPGA Bitstream 初始化（完全脫離 Windows VM）**：
   - [`progress.md` L656](../progress.md#L656) 提到之前 USBPcap 因預設 snaplen `65535` 截斷了 `411,334` bytes 的 FPGA Bitstream。
   - 其實不必回 Windows VM 重錄：FPGA Bitstream 本身就存在於 `DSWave.exe`（或其安裝目錄下的資源檔 / PE Resource 區段）中！而且當裝置剛插電（冷啟動）並在 Wine 下啟動 `DSWave.exe` 時，`DSWave.exe` 自己就會透過 `0x02` Bulk OUT 發送這筆 `411,334` bytes 的 Bitstream！只需在 [`helper.c`](../../wine-winusb-shim/src/helper.c#L728-L896) 的 [`do_bulk()`](../../wine-winusb-shim/src/helper.c#L728-L896) 攔截 `ep == 0x02 && len > 100000`，將 `c->in` 直接 `fwrite` 存成 `/tmp/dswave-fpga.bin` 即可在 Linux 下隨時重播。

---

## 四、建議執行步驟清單（Checklist）

1. **[P0 - 修復 `-EPROTO` 與 200ms URB 取消]**
   - 修改 [`helper.c`](../../wine-winusb-shim/src/helper.c#L235-L305) 的 [`read_descriptors()`](../../wine-winusb-shim/src/helper.c#L235-L263) 與 [`do_list()`](../../wine-winusb-shim/src/helper.c#L265-L305)：快取裝置描述元（或直接讀 `/sys/bus/usb/devices/.../descriptors`），**禁止在 `REQ_LIST` 中呼叫 `libusb_open()` / `libusb_control_transfer()` / `libusb_close()`**。
   - 修改 [`setupapi_shim.c`](../../wine-winusb-shim/src/setupapi_shim.c#L179-L212) 的 [`refresh_dev_identity()`](../../wine-winusb-shim/src/setupapi_shim.c#L179-L212)：加入 2 秒快取節流，避免每 100 ms 發送兩次 `REQ_LIST`。
   - 修改 [`helper.c`](../../wine-winusb-shim/src/helper.c#L755-L845) 的 [`do_bulk()`](../../wine-winusb-shim/src/helper.c#L728-L896)：廢除 `DSW_SLICE_MS = 200` 的 `libusb_bulk_transfer` 切片迴圈，改用單一非同步 `libusb_submit_transfer`（timeout 設為完整的 `req->d`）搭配 `libusb_handle_events_timeout_completed` 輪詢 `abort_gen`，確保正常傳輸期間 **0 次 URB Discard**。
2. **[P1 - 修復 STOP 流程與狀態碼回傳]**
   - 移除 [`helper.c`](../../wine-winusb-shim/src/helper.c#L548-L560) [`do_control()`](../../wine-winusb-shim/src/helper.c#L532-L600) 收到 `01 00 00` 時提早強制取消 `0x86` 的邏輯。
   - 修改 [`winusb_shim.c`](../../wine-winusb-shim/src/winusb_shim.c#L380-L410) 的 [`run_bulk_worker()`](../../wine-winusb-shim/src/winusb_shim.c#L332-L420)：取消時統一寫入 `STATUS_CANCELLED` (`0xC0000120`) 而非 `0xC0010000`。
   - 在 [`do_close()`](../../wine-winusb-shim/src/helper.c#L510-L530) 補上 `dsw_hw_soft_reset()`（發送 `01 00 00` -> `02 08 00` -> 讀 `0x84` -> `01 00 00` -> `02 04 00`），確保即使 App 異常中斷，FPGA FIFO 也能回到乾淨狀態。
