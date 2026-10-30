# `plan_stage4.md` 審查意見 (`argon_reply4.md`)

## 一、總體結論：**同意依此計畫執行（可進入實作階段）**

審查 [`plan_stage4.md`](plan_stage4.md) 後，確認這份計畫的架構設計、根因定位與驗證矩陣非常完整，**可以直接作為 Stage 4 的實作藍圖**。

特別肯定 [`plan_stage4.md` 第三節](plan_stage4.md#L66-L75) 對 `argon_reply3.md` 提出的五項修正：
1. **改用固定大小 Worker Pool（8–16 執行緒）+ 佇列**：避免連續擷取時反覆 `CreateThread` 造成執行緒與連線爆量。
2. **直接使用 `shim_req.b` 傳遞 32-bit timeout**：對齊 [`proto.h`](../../wine-winusb-shim/src/proto.h#L21-L27) 現有結構，無須拆分 `b`/`c`。
3. **對齊 `libusb` 的 `HasOverlappedIoCompleted` 機制**：確保 Worker 完成時正確寫入 `overlapped->Internal` 與 `overlapped->InternalHigh` 後再 `SetEvent(overlapped->hEvent)`。
4. **納入 Handle 生命週期保護（Refcount）**：避免 Control 連線關閉時誤殺仍在飛行中的 Bulk 傳輸。
5. **逾時由 `libusb` 的 `WinUsb_AbortPipe` 主導**：`helper` 端改用短切片輪詢 + 5 分鐘安全上限。

---

## 二、實作前必看的 4 個關鍵細節補正（避免 Race Condition 與掉包）

在動手寫 [`helper.c`](../../wine-winusb-shim/src/helper.c) 與 [`winusb_shim.c`](../../wine-winusb-shim/src/winusb_shim.c) 時，請將以下 4 個實作細節一併納入，可避免非同步取消時的 Use-After-Free 崩潰與 200ms 切片造成的波形錯位：

### 1. 釐清為何先前會出現 `LIBUSB_ERROR_IO`，以及 `WinUsb_ReadPipe` 的 `transferred == NULL` 陷阱

- **為何先前 log 出現 `LIBUSB_ERROR_IO`（並非硬體真 Stall）**：
  檢視 [`winusb_shim.c`](../../wine-winusb-shim/src/winusb_shim.c#L486-L515)，先前 `bulk_transfer()` 同步卡住 60 秒後收到 `helper` 回傳 `status = -7` (`LIBUSB_ERROR_TIMEOUT`)，卻呼叫 `SetLastError(ERROR_GEN_FAILURE)` 並回傳 `FALSE`。
  因為 `libusb` 的 `winusbx_submit_bulk_transfer()` 預期 `WinUsb_ReadPipe` 只會回傳 `TRUE` 或 `FALSE + ERROR_IO_PENDING`，看到 `FALSE + ERROR_GEN_FAILURE` 就直接當作提交失敗回傳 `LIBUSB_ERROR_IO`，觸發上層呼叫 `libusb_clear_halt` 重試。
- **`transferred == NULL` 導致 `InternalHigh` 恆為 0 的 Bug**：
  `libusb` 呼叫 `WinUsb_ReadPipe(h, ep, buf, len, NULL, overlapped)` 時，第 5 個參數 **`transferred` 傳的是 `NULL`**！
  現行 [`winusb_shim.c` L510](../../wine-winusb-shim/src/winusb_shim.c#L510) 寫了：
  ```c
  overlapped->InternalHigh = transferred ? *transferred : 0;
  ```
  這導致只要 `transferred == NULL`，同步路徑就算讀到資料也會把 `InternalHigh` 設成 `0`！
  - **修正建議**：在 [`plan_stage4.md` 5.3 第 3 點](plan_stage4.md#L211) 的「queue 滿時退回同步」路徑中，務必用區域變數 `ULONG actual = 0;` 接回 `resp.len` 並寫入 `overlapped->InternalHigh = actual;`，不可依賴 `transferred` 指標。

---

### 2. `WinUsb_AbortPipe` 的 Use-After-Free 風險（[`plan_stage4.md` 5.3 第 4 點](plan_stage4.md#L215-L225)）

計畫中寫道：
```c
for each pending ctx on (dev, pipe):
    ctx->cancelled = 1;
    ctx->overlapped->Internal = STATUS_CANCELLED;
    ctx->overlapped->InternalHigh = 0;
    SetEvent(ctx->overlapped->hEvent);
send REQ_ABORT_PIPE (background, 不阻塞)
```

- **潛在崩潰風險**：
  在 `libusb` 中，一旦 `overlapped->Internal` 變為非 `STATUS_PENDING` 且 `hEvent` 被 `SetEvent`，`libusb` 的事件迴圈就會立即喚醒並**釋放該 `libusb_transfer` 的 `buffer` 與 `overlapped` 記憶體**。
  如果此時某個 Worker Thread 正在執行這筆 `ctx`（卡在 `recv_all()` 等待 `helper` 的 200ms 切片結束），當 Worker 在幾十毫秒後收到回應並寫入 `ctx->buffer` 或 `ctx->overlapped->Internal` 時，就會發生 **Use-After-Free 記憶體損毀**！
- **修正建議**：將佇列中的請求區分為 **「尚未派發（Queued）」** 與 **「執行中（In-Flight）」** 兩種狀態：
  1. **Queued（尚未被 Worker 取走）**：在 `WinUsb_AbortPipe` 內直接從佇列移除，由 `WinUsb_AbortPipe` 設定 `overlapped->Internal = STATUS_CANCELLED`、`SetEvent(overlapped->hEvent)` 並釋放 `ctx`。
  2. **In-Flight（已被 Worker 取走、正在等待 `helper`）**：
     - `WinUsb_AbortPipe` **只標記 `ctx->cancelled = 1`**，並透過控制連線同步送出 `REQ_ABORT_PIPE`（`helper` 端只是改 flag，耗時 `< 0.1 ms`），**千萬不要在 `WinUsb_AbortPipe` 內直接改 In-Flight 的 `ctx->overlapped` 或 `SetEvent`**！
     - 讓 Worker Thread 在 `<= 200 ms` 內收到 `helper` 的 `LIBUSB_ERROR_INTERRUPTED` 返回後，**由 Worker Thread 獨自完成** `ctx->overlapped->Internal = STATUS_CANCELLED` 與 `SetEvent(ctx->overlapped->hEvent)`。這樣可 100% 保證 `buffer` 與 `overlapped` 在 Worker 結束前絕不會被 `libusb` 提前釋放。

---

### 3. `helper.c` 的 `abort_flags` 改用遞增序號（Generation Counter），避免被後續請求清掉（[`plan_stage4.md` 5.2 第 2 點](plan_stage4.md#L139-L158)）

計畫中寫道：
> `do_abort_pipe()`：`abort_flags[id] = 1`。
> `do_bulk()`：每次 transfer 前清除 flag；以 200 ms 切片迴圈

- **潛在競爭風險**：
  當 DSWave 預先提交多筆 Bulk IN（Ring Buffer）時，會有多個 Worker 同時或接續進入 `do_bulk()`。如果 `do_bulk()` 在開頭執行 `abort_flags[id] = 0`，當 `do_abort_pipe()` 設為 `1` 時，下一筆剛進來的 `do_bulk()` 會立刻把 `abort_flags[id]` 清回 `0`，導致前一筆正在跑 200ms 迴圈的 `do_bulk()` 看不到取消訊號！此外，`AbortPipe` 是針對特定 Endpoint（`req->a`）。
- **修正建議**：改用 **Per-(Handle, Endpoint) 的遞增計數器（Generation Counter）**，完全不需要「清除 flag」：
  ```c
  static volatile uint32_t abort_gen[MAX_HANDLES][256];
  ```
  - **`do_abort_pipe()`**：
    ```c
    uint8_t ep = (uint8_t)req->a;
    __sync_add_and_fetch(&abort_gen[id][ep], 1);
    ```
  - **`do_bulk()`**：
    進入迴圈**前**先拍下快照：
    ```c
    uint8_t ep = (uint8_t)req->a;
    uint32_t my_gen = __sync_fetch_and_add(&abort_gen[id][ep], 0);
    ```
    在 200ms 迴圈內檢查：
    ```c
    if (__sync_fetch_and_add(&abort_gen[id][ep], 0) != my_gen) {
        ret = LIBUSB_ERROR_INTERRUPTED;
        break;
    }
    ```
    如此一來，所有在 `AbortPipe` 之前送出的傳輸都會看到 `abort_gen != my_gen` 而立即中止，之後送出的新傳輸則會拿到新的 `my_gen`，完全不會互相覆蓋。

---

### 4. `do_bulk()` 200ms 切片的「同 Endpoint 順序保護」與「部分讀取累積（`transferred > 0`）」

在 [`plan_stage4.md` 5.2 第 2 點](plan_stage4.md#L148-L158) 的 200ms 切片迴圈中：
```c
ret = libusb_bulk_transfer(h, ep, buf, len, &transferred, slice);
```
有兩個小細節需要注意：

1. **避免多個 Worker 對同一個 Endpoint (`0x86`) 同時跑 200ms 切片造成亂序**：
   若 DSWave 一次掛 8 筆 `WinUsb_ReadPipe(0x86)`，而 8 個 Worker 同時連進 `helper` 對 `0x86` 呼叫 `libusb_bulk_transfer(..., 200)`，每 200ms 逾時重試時 Linux 執行緒排程順序會洗牌，可能導致第 3 筆 `OVERLAPPED` 先拿到第一包波形資料（造成 Ring Buffer 順序錯亂）。
   - **修正建議**：在 `helper.c` 為每個 `(handle, ep)` 加一把細粒度鎖 `pthread_mutex_t ep_lock[MAX_HANDLES][256]`（先讀取 `my_gen = abort_gen[id][ep]`，再 `pthread_mutex_lock(&ep_lock[id][ep])` 進入 200ms 迴圈）。
   - 這樣不同 Endpoint（例如 `0x00` 控制、`0x02` Bulk OUT、`0x86` Bulk IN、`0x84` Bulk IN）之間**完全平行無鎖**，而同一個 `0x86` 內部的多筆 `OVERLAPPED` 請求則保證**嚴格按照 FIFO 順序**依序填入資料並喚醒事件！
2. **200ms 切片逾時但已讀到部分資料（`ret == LIBUSB_ERROR_TIMEOUT && transferred > 0`）**：
   若 4096 bytes 傳到一半（例如剛收到 1024 bytes）剛好跨過 200ms 切片邊界，`libusb_bulk_transfer` 會回傳 `LIBUSB_ERROR_TIMEOUT` 但 `transferred = 1024`。若直接重跑迴圈傳入 `buf` 與 `len`，前 1024 bytes 會被覆蓋掉。
   - **修正建議**：在迴圈內維護 `uint32_t total = 0;`，每次呼叫：
     ```c
     int xfer = 0;
     ret = libusb_bulk_transfer(h, ep, buf + total, len - total, &xfer, slice);
     if (xfer > 0)
         total += (uint32_t)xfer;
     if (ret == 0 || total >= len) {
         ret = 0;
         break;
     }
     if (ret != LIBUSB_ERROR_TIMEOUT)
         break;
     ```
     最後回傳 `resp.len = total;`，即可確保跨 200ms 邊界時絕不漏掉任何一個 byte。

---

## 三、總結：建議直接按計畫第十節執行順序動工

[`plan_stage4.md` 第十節](plan_stage4.md#L307-L315) 的六個步驟：
1. 更新 [`proto.h`](../../wine-winusb-shim/src/proto.h)（`REQ_ABORT_PIPE` / `REQ_RESET_PIPE`）
2. 更新 [`helper.c`](../../wine-winusb-shim/src/helper.c)（縮小 `usb_lock`、200ms 切片 + `abort_gen` + `ep_lock`、日誌）
3. 執行 `helper` 自測與 Abort 測試
4. 更新 [`winusb_shim.c`](../../wine-winusb-shim/src/winusb_shim.c)（Async `OVERLAPPED` + Worker Pool + `AbortPipe` / `ResetPipe`）
5. 重裝並執行 V4.1 ～ V4.6 驗證
6. 視 V4.4 結果決定是否進入第七節備援排查

規劃非常清楚且風險可控，請直接依此計畫推進實作！
