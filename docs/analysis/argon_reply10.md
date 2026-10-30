# Stage 4 第二階段審查與 Stop 卡死根因剖析 (`argon_reply10.md`)

針對 [progress.md](../progress.md#L673-L709)（「2026-10-30 下午 第二階段：EPROTO 已解、Stop 仍會卡死」），本報告結合 `/tmp/dswave-helper.log`、`winusb.log` 與 `DSWave.exe` 反組譯結果，完整還原按下 **Stop** 後卡死 28 秒的真實控制流與兩個核心根因，並提供可直接套用的修正方案。

---

## 一、第二階段成果確認

1. **描述子快取與 2 秒節流（P0-1）完全生效**：
   - [`refresh_dev_identity`](../../wine-winusb-shim/src/setupapi_shim.c#L473-L511) 的 2 秒節流搭配 [`helper.c`](../../wine-winusb-shim/src/helper.c#L329-L361) 的 `cached_dev_info`，使整段 Session 的 `REQ_LIST`（`req=1`）從 **1,627 次降至 6 次**，背景熱插拔輪詢不再碰觸 `libusb` 或 FX3 EP0。
2. **`do_bulk` 改為非同步 `libusb_submit_transfer` + 50ms 輪詢（P0-2）徹底消除 `-EPROTO`**：
   - [`do_bulk`](../../wine-winusb-shim/src/helper.c#L747-L883) 不再以 200ms 切片週期性 cancel URB，**連續 30 秒採集期間 `status = -71 (-EPROTO)` 與 `LIBUSB_ERROR_IO (-1)` 皆為 0 次**，硬體與推進迴圈的資料流穩定度已完全達標。

---

## 二、Stop 卡死之完整根因剖析（Log 時間軸 + `DSWave.exe` 反組譯）

### 1. 關鍵時間軸澄清：`t = 172459.818` 的 `seq=28 (-10)` 不是 Stop 觸發，而是強關程式觸發！

對照 [`winusb.log`]($WINEPREFIX/drive_c/ProgramData/DSWaveShim/winusb.log) 與 `/tmp/dswave-helper.log` 的時間戳：

| 時間戳 (`winusb.log` / `helper.log`) | 事件與對應函式 | 說明 |
| :--- | :--- | :--- |
| `172429.277` (`[44.448801]`) | `ReadPipe(0x86, len=5005312)` `seq=28` 開始 | `seq=28` 前 11 個 chunk（共 `4,460,544` bytes）正常收完；第 12 個 chunk（`n=12, 405,504` bytes）於 `[46.821633]` 提交。同時 `seq=29` (`d=001d`) 已進入 [`do_bulk`](../../wine-winusb-shim/src/helper.c#L767) 並卡在 `pthread_mutex_lock(&ep_lock[id][0x86])`。 |
| **`172431.650`** | **使用者點擊 Stop** | UI 執行緒 (`0x1402ad950`) 呼叫 `0x1402ad8c0` 設定 `stop_flag` (`[rbx+0x90] = 1`)，接著停在 `std::thread::join(&TransferThread)` (`0x1402ad9d6`)。 |
| **`172431.650`** | `TransferThread` (`0x1402ae030` -> `0x1402adfb0`) 執行 `UsbBsp3x::stop` (`0x1402be630`) | 送出兩筆 EP0 Vendor Control：<br>1. `01 00 00` (`seq=33`, Stop acquisition)<br>2. `02 08 00` (`seq=34`, Flow pause)<br>並 `join` 結束 `RegQueueThread`（`EP 0x84` `seq=35` 於 `172431.653` 正常返回）。 |
| **`172431.653` ~ `172459.818`** | **卡死 28.165 秒** | `0x1402adfb0` 尾呼叫 `UsbTransfer::abort` (`0x1402b40b0`)，並在 `TransferThread` 迴圈 (`0x1402ae167` -> `0x1402b41c0`) 中不斷呼叫 `libusb_handle_events_timeout_completed`，**等待 `seq=28` 與 `seq=29` 完成後才肯呼叫 `UsbBsp3x::reset` (`0x1402be900`, 送出 `02 04 00`)**。 |
| **`172459.818`** (`[74.989677]`) | 使用者強制關閉 `DSWave.exe` | 行程關閉導致 socket EOF，觸發 [`close_owned`](../../wine-winusb-shim/src/helper.c#L964-L977) -> [`do_close`](../../wine-winusb-shim/src/helper.c#L575-L580) 遞增 `abort_gen` 並設 `close_pending=1`，這才喚醒 `seq=28`（收割 `53,248` bytes 並回傳 `-10`）與 `seq=29`（`acquire_handle` 失敗回傳 `status=-2`）。 |

---

### 2. 根因 A（Helper 端）：`DSWave.exe` 與 `helper.c` 在 Stop 流程產生「循環等待死結」

反組譯 `DSWave.exe` 的 `TransferThread` (`0x1402ae030`)、Stop 輔助函式 (`0x1402adfb0`) 與 `UsbBsp3x` 虛擬表 (`0x1402be630` / `0x1402be900`) 後，發現雙方的狀態機存在明確的循環依賴：

1. **`DSWave.exe` 的順序**：
   - 在 `0x1402adfb0` 中，先呼叫 `UsbBsp3x::vtable[0xa0]` (`0x1402be630`) 送出 **`01 00 00`（Stop）** 與 **`02 08 00`（Flow pause）**，接著呼叫 `UsbTransfer::abort` (`0x1402b40b0`)。
   - 隨後 `TransferThread` 回到 `0x1402ae167` 輪詢 `UsbTransfer::poll` (`0x1402b41c0`)，**必須等到所有 in-flight 的 `EP 0x86` Bulk IN 傳輸（`seq=28`、`seq=29`）都結束、狀態欄位 `[rcx+0x30]` 變為 `5`（Stopped）之後**，才會跳到 `0x1402ae211` 呼叫 `UsbBsp3x::vtable[0xa8]` (`0x1402be900`) 送出 **`02 04 00`（`"BSP: Resetting hardware logic..."`）**！
2. **`helper.c` 的順序**：
   - 在 [`do_control`](../../wine-winusb-shim/src/helper.c#L665-L681) 中，目前**只針對 `02 04 00`（`data[0] == 0x02 && data[1] == 0x04`）遞增 `abort_gen[id]`**，對 `01 00 00` 與 `02 08 00` 完全沒有觸發任何中止或排空（drain）機制！
   - 當硬體在 `172431.650` 收到 `01 00 00` 與 `02 08 00` 後，硬體把 FIFO 剩餘的 **`53,248` bytes** 吐完就停止發送。
   - 偏偏 **`53,248 = 104 × 512` 是 USB 2.0 High-Speed Bulk `wMaxPacketSize`（`512` bytes）的整數倍**！因為硬體沒有補送 ZLP（Zero-Length Packet）或短封包，Linux `usbfs` 認為 `405,504` bytes 的第 12 個 chunk 還沒收滿，於是 URB 就死卡在 kernel 裡等待剩下的 `352,256` bytes（直到 300 秒 timeout）。
   - 結果：**`DSWave.exe` 等 `seq=28`/`seq=29` 結束才要送 `02 04 00`，而 `helper.c` 等收到 `02 04 00` 才要透過 `abort_gen` 取消 `seq=28`/`seq=29`**，形成完美的死結！

---

### 3. 根因 B（Helper 端）：`seq=29` 在 `ep_lock` 之後才讀取 `my_abort`，導致排隊中的傳輸無法感知 Abort

在 [`do_bulk`](../../wine-winusb-shim/src/helper.c#L767-L768) 中：

```c
pthread_mutex_lock(&ep_lock[id][ep]);
uint32_t my_abort = atomic_load(&abort_gen[id]);
```

- 採集期間 `DSWave.exe` 會同時提交多個 `EP 0x86` 傳輸（例如 `seq=28` 正在跑 URB 時，`seq=29` 已經進入 `do_bulk` 並卡在 `pthread_mutex_lock(&ep_lock[id][0x86])`）。
- 如果在 `seq=28` 執行期間遞增了 `abort_gen[id]`：
  1. `seq=28` 偵測到 `atomic_load(&abort_gen[id]) != my_abort` 而取消 URB 並釋放 `ep_lock`。
  2. 這時排隊中的 `seq=29` 取得 `ep_lock`，並在**鎖內（Line 768）把更新後的 `abort_gen[id]` 讀進自己的 `my_abort`**！
  3. 結果 `seq=29` 完全不知道剛剛發生過 Abort，會在已經停止的硬體上再度送出新的 `405,504` bytes URB 並再次卡死！

---

### 4. 根因 C（Wine 端）：`windows_cancel_transfer` 依賴 `CancelIoEx`，而 `hook_CancelIoEx` 被 `GetFinalPathNameByHandleW` 擋掉

反組譯 `DSWave.exe` 內建的 `libusb` Windows 後端 (`windows_cancel_transfer` @ `0x140331c90` 與 `winusb_abort_transfers` @ `0x140334da0`) 發現：

1. 當 `UsbTransfer::abort` (`0x1402b40b0`) 呼叫 `libusb_cancel_transfer` (`0x14032e990`) 時，`windows_cancel_transfer` (`0x140331c90`) **第一步是直接透過 IAT 呼叫 `KERNEL32.dll!CancelIoEx(transfer_priv->handle, &transfer_priv->overlapped)` (`0x140331cb4`)**。
2. 如果 `CancelIoEx` 回傳 `FALSE` 且 `GetLastError() == ERROR_NOT_FOUND (0x490)` (`0x140331cd6`)，`windows_cancel_transfer` 會直接認定該傳輸已不在佇列中，立刻回傳 `LIBUSB_ERROR_NOT_FOUND (-5)`，**根本不會呼叫後面的 `winusb_abort_transfers`（即 `WinUsb_AbortPipe`）**！
3. 再看 [`ioctl_hook.c:hook_CancelIoEx`](../../wine-winusb-shim/src/ioctl_hook.c#L336-L356)：
   ```c
   static BOOL WINAPI hook_CancelIoEx(HANDLE h, LPOVERLAPPED ov) {
       WCHAR path[MAX_PATH];
       if (h && h != INVALID_HANDLE_VALUE &&
           GetFinalPathNameByHandleW(h, path, MAX_PATH, 0) &&
           wcsstr(path, L"DSWaveShim")) {
           typedef BOOL (WINAPI *pfn_cancel_ov_t)(LPOVERLAPPED);
           HMODULE wu = GetModuleHandleA("winusb.dll");
           pfn_cancel_ov_t fn = wu ? (pfn_cancel_ov_t)GetProcAddress(wu, "shim_cancel_overlapped") : NULL;
           BOOL done = fn && ov ? fn(ov) : FALSE;
           cancel_log("CancelIoEx(h=%p, ov=%p) -> shim_cancel=%d\n", h, (void*)ov, done);
           ...
       }
       return real_cancelioex ? real_cancelioex(h, ov) : FALSE;
   }
   ```
   - `cancel_log` 被放在 `if (GetFinalPathNameByHandleW(...) && wcsstr(path, L"DSWaveShim"))` **裡面**！只要 `h` 在 Wine 下無法通過 `GetFinalPathNameByHandleW`（或 `libusb` 傳入的 `transfer_priv->handle` 與預期不同），`hook_CancelIoEx` 就會**連一行 Log 都不印**，直接落入 Wine 原生的 `real_cancelioex(h, ov)`。
   - 而 Wine 原生的 `real_cancelioex` 在 dummy 檔案 handle 上找不到任何 Wine server 層級的 async I/O，必然回傳 `FALSE` + `ERROR_NOT_FOUND (0x490)`，導致 `libusb_cancel_transfer` 靜默失敗！
   - 事實上，[`shim_cancel_overlapped(ov)`](../../wine-winusb-shim/src/winusb_shim.c#L367-L382) 本身就會在 `pending_head` 鏈結串列中比對 `e->ov == ov`，因此當 `ov != NULL` 時，**根本不需要先用 `GetFinalPathNameByHandleW(h, ...)` 檢查 `h`**！

---

## 三、具體修改建議（P0 必做）

### P0-1：修正 [`helper.c`](../../wine-winusb-shim/src/helper.c) 的 Stop 觸發條件與 `ep_lock` 排隊感知

有三個關鍵修改點：
1. **在 [`do_control`](../../wine-winusb-shim/src/helper.c#L665-L681) 中，除了 `02 04 00`（Reset）之外，當收到 `01 00 00`（Stop）或 `02 08 00`（Flow pause）時，也觸發 `abort_gen[id]`**（或者為 `01 00 00` / `02 08 00` 啟動短暫 30ms drain 後觸發 `abort_gen`）：
   - 實務上，當 `DSWave.exe` 在 `0x1402be630` 送出 `01 00 00` 與 `02 08 00` 時，它緊接著在幾微秒內就會呼叫 `UsbTransfer::abort` (`0x1402b40b0`) 丟棄剩餘未完成的傳輸。
   - 因此，在 [`do_control`](../../wine-winusb-shim/src/helper.c#L665-L681) 的 `libusb_control_transfer` 完成後，若偵測到 `req->bRequest == 0xb0` 的 Stop (`01 00 00`)、Flow pause (`02 08 00`) 或 Reset (`02 04 00`)，直接遞增 `abort_gen[id]` 即可立即喚醒 [`do_bulk`](../../wine-winusb-shim/src/helper.c#L822-L826)！
2. **在 [`do_bulk`](../../wine-winusb-shim/src/helper.c#L766-L775) 中，必須在 `pthread_mutex_lock(&ep_lock[id][ep])` **之前**讀取 `my_abort`**：
   - 取得 `ep_lock` 後立刻檢查 `atomic_load(&abort_gen[id]) != my_abort`。若不相等，代表在排隊等鎖期間發生了 Stop/Abort，應直接跳過 URB 提交並回傳 `LIBUSB_ERROR_INTERRUPTED (-10)`，徹底防止 `seq=29` 在 Stop 後還送出新 URB。
3. **在 [`do_bulk`](../../wine-winusb-shim/src/helper.c#L862-L872) 取消 URB 後的回傳值處理**：
   - 當因為 `abort_gen`（Stop/Reset）而取消正在進行的 chunk（如 `seq=28` 的第 12 個 chunk），且前面幾個 chunk 或剛收割的 URB 已經累積了 `transferred > 0`（例如 `4,513,792` bytes）時，或是 `r == LIBUSB_ERROR_INTERRUPTED` 時，[`pipe_worker`](../../wine-winusb-shim/src/winusb_shim.c#L911-L928) 會將 `r == -10` 轉為 `ERROR_OPERATION_ABORTED (995)` 並喚醒 `ov->hEvent`，`DSWave.exe` 的 `TransferThread` 就能在幾毫秒內收到完成通知並推進到 `[rcx+0x30] == 5`！

#### 建議修改片段（[`wine-winusb-shim/src/helper.c`](../../wine-winusb-shim/src/helper.c)）：

```c
/* 在 do_control (約 line 665-681) 中擴充 Stop / Flow pause 偵測： */
    int is_stop_or_reset = 0;
    if ((req->bmRequestType & 0x80) == 0 && req->bRequest == 0xb0 &&
        req->wLength == 3 && data != NULL) {
        if ((data[0] == 0x02 && data[1] == 0x04) || /* Reset hardware logic */
            (data[0] == 0x01 && data[1] == 0x00) || /* Stop acquisition */
            (data[0] == 0x02 && data[1] == 0x08)) { /* Flow pause */
            is_stop_or_reset = 1;
        }
    }
```

並且在 `do_control` 的 `libusb_control_transfer` 執行完畢後（約 line 690）：

```c
    int r = libusb_control_transfer(h, req->bmRequestType, req->bRequest,
                                    req->wValue, req->wIndex,
                                    data, req->wLength, req->timeout_ms);
    if (is_stop_or_reset && id >= 0 && id < MAX_DEV) {
        uint32_t gen = atomic_fetch_add(&abort_gen[id], 1) + 1;
        if (verbose)
            fprintf(stderr, "helper: stop/reset cmd (%02x %02x %02x) on id=%d -> abort_gen=%u\n",
                    data[0], data[1], data[2], id, gen);
    }
```

接著在 [`do_bulk`](../../wine-winusb-shim/src/helper.c#L766-L775) 中，將 `my_abort` 移到 `pthread_mutex_lock` **之前**：

```c
    /* Record abort_gen BEFORE waiting on ep_lock so queued bulk requests
     * (e.g. seq=29 waiting behind seq=28) immediately abort when seq=28
     * releases ep_lock after a Stop/Abort! */
    uint32_t my_abort = atomic_load(&abort_gen[id]);
    pthread_mutex_lock(&ep_lock[id][ep]);

    if (atomic_load(&abort_gen[id]) != my_abort || atomic_load(&close_pending[id])) {
        pthread_mutex_unlock(&ep_lock[id][ep]);
        release_handle(id);
        if (verbose)
            fprintf(stderr, "helper: bulk ep=0x%02x len=%u d=%04x aborted while queued on ep_lock\n",
                    ep, total_len, dtag);
        struct dsw_StatusHdr sh = { .status = LIBUSB_ERROR_INTERRUPTED, .actual_length = 0 };
        return send_reply(c, req->seq, &sh, sizeof(sh), NULL, 0);
    }
```

---

### P0-2：修正 [`ioctl_hook.c:hook_CancelIoEx`](../../wine-winusb-shim/src/ioctl_hook.c#L336-L356) 與 [`winusb_shim.c`](../../wine-winusb-shim/src/winusb_shim.c#L367-L382)，移除 `GetFinalPathNameByHandleW` 前置阻擋

為了讓 `DSWave.exe` 呼叫 `CancelIoEx(h, ov)` 時能 100% 命中我們的 shim 並留下診斷紀錄：
1. **當 `ov != NULL` 時，直接呼叫 `shim_cancel_overlapped(ov)`**：
   - 因為 `shim_cancel_overlapped(ov)` 只會在 `ov` 存在於 `winusb_shim.c` 的 `pending_head` 串列時才回傳 `TRUE`，不會誤傷任何非 shim 的一般檔案 I/O！
   - 如果 `shim_cancel_overlapped(ov)` 回傳 `TRUE`，直接設定 `SetLastError(ERROR_SUCCESS)` 並回傳 `TRUE`；這樣 `windows_cancel_transfer` (`0x140331c90`) 就會收到成功結果。
2. **無條件記錄 `cancel_log`（或至少在 `shim_cancel_overlapped` 命中及 `h` 匹配時記錄）**，方便確認 `DSWave.exe` 傳入的 `(h, ov)`。
3. **當 `shim_cancel_overlapped(ov)` 在 `pending_head` 找不到 `ov`（例如該傳輸剛好已經在 `pipe_worker` 完成並移出 `pending_head`），但 `h` 是 `DSWaveShim` handle 時**，應呼叫 `SetLastError(ERROR_NOT_FOUND)`（`0x490`）而非 `ERROR_INVALID_FUNCTION`，以符合 Windows `CancelIoEx` 與 `libusb` `windows_cancel_transfer` (`0x140331cd6`) 的規格。

#### 建議修改片段（[`wine-winusb-shim/src/ioctl_hook.c`](../../wine-winusb-shim/src/ioctl_hook.c#L336-L356)）：

```c
static BOOL WINAPI hook_CancelIoEx(HANDLE h, LPOVERLAPPED ov) {
    typedef BOOL (WINAPI *pfn_cancel_ov_t)(LPOVERLAPPED);
    HMODULE wu = GetModuleHandleA("winusb.dll");
    pfn_cancel_ov_t fn = wu ? (pfn_cancel_ov_t)GetProcAddress(wu, "shim_cancel_overlapped") : NULL;

    /* Fast path: if ov belongs to an in-flight WinUSB transfer in pending_head,
     * cancel it immediately regardless of what handle h libusb passed! */
    if (fn && ov && fn(ov)) {
        cancel_log("CancelIoEx(h=%p, ov=%p) -> matched pending_head! cancelled=1\n",
                   h, (void*)ov);
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    }

    WCHAR path[MAX_PATH];
    if (h && h != INVALID_HANDLE_VALUE &&
        GetFinalPathNameByHandleW(h, path, MAX_PATH, 0) &&
        wcsstr(path, L"DSWaveShim")) {
        cancel_log("CancelIoEx(h=%p, ov=%p) on DSWaveShim handle -> ov not in pending_head\n",
                   h, (void*)ov);
        SetLastError(ERROR_NOT_FOUND);
        return FALSE;
    }

    cancel_log("CancelIoEx(h=%p, ov=%p) -> passthrough to real_cancelioex\n",
               h, (void*)ov);
    return real_cancelioex ? real_cancelioex(h, ov) : FALSE;
}
```

---

## 四、驗證清單

套用上述 **P0-1** 與 **P0-2** 後，重新編譯 `helper`、`winusb.dll.so` 與 `setupapi.dll.so`，啟動 `DSWave.exe` 進行「Start 採集 10 秒 -> 點擊 Stop -> 再點擊 Start」測試，預期觀察到：
1. 點擊 **Stop** 瞬間（`< 50 ms` 內）：
   - `/tmp/dswave-helper.log` 印出 `helper: stop/reset cmd (01 00 00) on id=0 -> abort_gen=...`。
   - 正在進行的 `seq=28` 在 50ms 輪詢視窗內立刻偵測到 `abort_gen` 變化、取消 URB 並回傳 `-10`。
   - 排在 `ep_lock` 後方的 `seq=29` 取得鎖後立即看到 `abort_gen != my_abort`，印出 `aborted while queued on ep_lock` 並立刻回傳 `-10`，不再送出新 URB。
2. `DSWave.exe` 的 `TransferThread` 立即收齊 `seq=28` 與 `seq=29` 的完成事件，順利推進到 `0x1402be900` 送出 `02 04 00`（Reset hardware logic），UI 瞬間恢復可操作狀態，不再卡死！
