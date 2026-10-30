# 針對 `plan.md`（2026-10-30 資料流穩定性改進計畫）之審查意見 (`argon_reply9.md`)

審查完 [`plan.md`](../plan.md)，並進一步反組譯驗證 `DSWave.exe` 內部的 `libusb` 傳輸完成/取消路徑與 [`/tmp/dswave-helper.log`](file:///tmp/dswave-helper.log) 時間軸後，結論如下：

## 一、總體結論：**同意依照此計畫執行（附 3 項關鍵實作補強）**

[`plan.md`](../plan.md) 的目標設定、根因假設排序（H1–H5）、工作項目拆解（P0-1 → P0-2 → P1-1/P1-2/P1-3 → P2-1/P2-2）以及回滾策略都非常清晰且切中要害，**完全同意照此計畫推進**。

特別補充一個從 [`/tmp/dswave-helper.log`](file:///tmp/dswave-helper.log#L27715-L27737) 挖出的決定性證據，100% 印證計畫中 **P0-1** 的判斷：
- 在 [`/tmp/dswave-helper.log` L27715](file:///tmp/dswave-helper.log#L27715)（`12.316458s`），`seq=1` 剛開始讀取 `0x86`，僅在 **1.35 ms** 內就順利收到了 `335,872` bytes。
- 緊接著在 `12.317811s`（[L27727](file:///tmp/dswave-helper.log#L27727)，即 `seq=1` 啟動後第 **1.35 ms**），熱插拔輪詢 `REQ_LIST` 呼叫 `libusb_open()` 並送出 EP0 `GET_DESCRIPTOR`（`transfer 0x765c04000e60`）。
- **就在這筆 EP0 `GET_DESCRIPTOR` 送出的瞬間，FX3 的 EP0 與 `0x86` 同時卡死**：該筆 EP0 `GET_DESCRIPTOR` 在 1000 ms 後（`13.317872s`，[L27737](file:///tmp/dswave-helper.log#L27737)）因超時被取消；而 `0x86` 則凍結在 `335,872` bytes 直到 2000 ms 後（`14.316534s`，[L29120-L29135](file:///tmp/dswave-helper.log#L29120-L29135)）超時取消，並在下一輪立刻報 `urb status = -71` (`-EPROTO`，[L29167](file:///tmp/dswave-helper.log#L29167))！
- 因此，**P0-1 + P0-2 同輪修改並測試**，直接打中了真正的病灶。

---

## 二、執行 P0-2 / P1-1 / P1-2 時必須加入的 3 項關鍵補強（避免死鎖與 Use-After-Free）

為了確保依計畫實作時一次成功，請在寫 code 時納入以下三個透過反組譯 `DSWave.exe` 與 Wine `kernelbase.dll` 確認的細節：

### 1. 【補強 P1-1】為什麼 `WinUsb_AbortPipe` (`REQ_ABORT_PIPE`) 從未被觸發？需在 [`ioctl_hook.c`](../../wine-winusb-shim/src/ioctl_hook.c) 多掛鉤 `CancelIoEx`，否則 P0-2 + P1-1 在按 Stop 時必中 2 秒超時！

[`plan.md` P1-1 (L75-L81)](../plan.md#L75-L81) 規劃：
> 移除收到 `01 00 00` 時 `abort_gen++` 的行為；改為收到 `02 04 00` 或 `REQ_ABORT_PIPE` 才 abort（若 2 秒內沒等到 `02 04 00` 再補 abort）。

- **潛在死鎖原因**：
  對照 Windows [`/tmp/fpga1.csv` L2809-L2818](file:///tmp/fpga1.csv#L2809-L2818) 與 Wine [`winusb.log` L37503-L37507]($WINEPREFIX/drive_c/ProgramData/DSWaveShim/winusb.log#L37503-L37507)，並反組譯 `DSWave.exe` 的停止函式（`0x1402b40b0`）後確認，App 按下 Stop 時的執行順序是：
  1. 送出 `01 00 00` → `02 08 00`（以及讀取 `0x84`）；
  2. 設定 `session->stopping = 1`（`0x1402b40db`），並對所有飛行中的 `0x86` transfer 呼叫 **`libusb_cancel_transfer()`**（`0x1402b40f3`）；
  3. **阻塞等待 `0x86` 傳輸完成退回後**，才繼續送出第二筆 `01 00 00` 與 **`02 04 00`**！
  換言之，**在 `0x86` 被 abort 完成之前，App 永遠不會送出 `02 04 00`！**
- **為什麼 App 呼叫了 `libusb_cancel_transfer()`，墊片卻從沒收到 `WinUsb_AbortPipe`？**
  反組譯 `DSWave.exe` 的 `windows_cancel_transfer`（`0x140331c90`）發現：
  - 它在呼叫 `winusbx_cancel_transfer`（即 [`WinUsb_AbortPipe`](../../wine-winusb-shim/src/winusb_shim.c#L919-L947)）之前，會先呼叫 **`KERNEL32!CancelIoEx(hFile, overlapped)`**（`0x140331cb4`）。
  - 因為 `hFile` 是墊片開啟的假檔案 `dev.bin`，`wineserver` 中沒有任何核心層級的 pending I/O，所以 Wine 的 `CancelIoEx` 會回傳 `FALSE` 並設定 `LastError = ERROR_NOT_FOUND (0x490 = 1168)`。
  - 而 `windows_cancel_transfer` 在 `0x140331cd6` (`cmp $0x490, %eax`) 看到 `ERROR_NOT_FOUND` 時，會誤判該筆傳輸已經結束，直接回傳 `LIBUSB_ERROR_NOT_FOUND (-5)`，**根本不會走進 fallback 呼叫 `WinUsb_AbortPipe`！**
- **具體做法（僅需在 [`ioctl_hook.c`](../../wine-winusb-shim/src/ioctl_hook.c#L308-L338) 加 15 行）**：
  在 [`ioctl_hook_install()`](../../wine-winusb-shim/src/ioctl_hook.c#L271-L340) 攔截 `KERNEL32.dll` IAT 時，順便掛鉤 `CancelIoEx`：
  ```c
  typedef BOOL (WINAPI *cancel_io_ex_t)(HANDLE, LPOVERLAPPED);
  static cancel_io_ex_t real_cancel_io_ex;

  static BOOL WINAPI hook_CancelIoEx(HANDLE h, LPOVERLAPPED ov)
  {
      if (!hub_handle(h))
      {
          /* 回傳非 ERROR_NOT_FOUND (0x490)，讓 libusb 的 windows_cancel_transfer
           * 自動 fallback 呼叫 winusbx_cancel_transfer -> WinUsb_AbortPipe！ */
          SetLastError(ERROR_INVALID_FUNCTION);
          return FALSE;
      }
      return real_cancel_io_ex ? real_cancel_io_ex(h, ov) : FALSE;
  }
  ```
  加上這個 Hook 後，當 App 在 `02 08 00` 之後呼叫 `libusb_cancel_transfer()` 時，就會自動觸發 [`WinUsb_AbortPipe(h, 0x86)`](../../wine-winusb-shim/src/winusb_shim.c#L919-L947) → `REQ_ABORT_PIPE`，完全不必依賴猜測控制命令或苦等 2 秒超時！（P1-1 在 `02 04 00` 時補 abort 仍可保留作為雙重保險）。

---

### 2. 【補強 P0-2】非同步 `libusb_submit_transfer` 取消時的生命週期（防止 Use-After-Free）與 `chunk` 設定

[`plan.md` P0-2 (L67-L73)](../plan.md#L67-L73) 改用 `libusb_submit_transfer()` 搭配 `libusb_handle_events_timeout_completed(ctx, 50ms, &done)` 迴圈，這是消除 200ms 切片 URB Discard 的最標準做法。實作時請特別注意兩點：

1. **取消後的 Reap 等待（防止 UAF 崩潰）**：
   當迴圈偵測到 `abort_gen` 變更而呼叫 `libusb_cancel_transfer(xfer)` 後，**絕對不能立刻 `break` 並 `free(buf)` / `libusb_free_transfer(xfer)`**！
   因為 `libusb_cancel_transfer()` 只是非同步向核心發出 `USBDEVFS_DISCARDURB`，核心 xHCI 尚未退回 URB；必須設一個旗標 `cancelling = 1`（避免每輪重複呼叫 `libusb_cancel_transfer`），並**繼續留在 `libusb_handle_events_timeout_completed` 迴圈中直到 callback 被觸發、設定 `done = 1`** 後，才能釋放 `xfer` 與 `buf`。
2. **`chunk` 與 `5,005,312` 的整除問題**：
   - 請注意 `5,005,312` **不是** `405,504` 的整數倍（`5,005,312 = 12 × 405,504 + 139,264`）。
   - 反組譯 `DSWave.exe` 的 `receive_data_callback`（`0x1402b4df0`）已確認：除第一筆 `4096B` header 外，資料讀取 callback（`0x1402b4e5b-0x1402b4e6d`）只檢查 `status == LIBUSB_TRANSFER_COMPLETED && actual_length > 0`，**不要求單筆必須湊滿 `5,005,312` bytes**（這也是為何 `fpga1.pcap` 每筆 `405,504` bytes 都能正常顯示）。
   - 若直接將單筆 `libusb_submit_transfer` 的長度設為 `405,504`（當 `req->len > 405504` 時，即 `DSW_FILL=0, DSW_CHUNK_BYTES=405504`），或直接一次提交 `req->len` 讓 Linux `libusb` 自己在核心內切 URB，都能保證在完成前 **0 次 URB Cancel**。建議優先以每次完成一個 `405,504`（`DSW_FILL=0`）或單次提交完整 `req->len` 測試，避免在 `do_bulk` 裡分 13 次提交而讓第 13 次（`139,264` bytes）切斷硬體的 `405,504` 封包邊界。

---

### 3. 【驗證 P1-2】Wine `KERNEL32!GetOverlappedResult` 與 `STATUS_CANCELLED` 的對應關係已 100% 確認

反組譯 `DSWave.exe` 的 `windows_handle_transfer_completion`（`0x140331ef8`）與 Wine 的 `kernelbase!GetOverlappedResult`（`0x17402c840`）確認：
1. `DSWave.exe` 呼叫的是 `KERNEL32!GetOverlappedResult`（而非 `WinUsb_GetOverlappedResult`）。
2. Wine 的 `GetOverlappedResult` 會讀取 `esi = (DWORD)ov->Internal`，執行 `SetLastError(RtlNtStatusToDosError(esi))`，並回傳 `(esi == STATUS_SUCCESS || esi == STATUS_PENDING)`。
3. 當 **P1-2** 將取消狀態改為 **`STATUS_CANCELLED` (`0xC0000120`)** 時：
   - Wine `RtlNtStatusToDosError(0xC0000120)` 會回傳 `ERROR_OPERATION_ABORTED` (`995 = 0x3e3`)；
   - `DSWave.exe` 在 `0x140331fe3` (`cmp $0x3e3, %ebx`) 就會精準命中取消分支，呼叫 `usbi_handle_transfer_cancellation` 回報 `LIBUSB_TRANSFER_CANCELLED (3)`！
4. 當發生真實 I/O 錯誤時，填入 **`STATUS_DEVICE_DATA_ERROR` (`0xC000009C`)** 會轉為 `ERROR_CRC (23)`，在 `DSWave.exe` 中回報為 `LIBUSB_TRANSFER_ERROR (1)`（若填 `STATUS_UNSUCCESSFUL = 0xC0000001` 則會轉為 `ERROR_GEN_FAILURE = 31`，在 `0x14033206a` 被視為 `LIBUSB_TRANSFER_STALL (4)`，兩者皆可正常觸發錯誤處理而不會被誤判為 `COMPLETED; length 0`）。

---

## 三、總結建議

請直接按照 [`plan.md`](../plan.md) 的步驟（**P0-1 + P0-2 → P1-1 + P1-2 + `hook_CancelIoEx` → P1-3 → P2-1/P2-2**）執行。這份計畫結構完整、驗證指標明確，加上上述 `hook_CancelIoEx` 與非同步取消等待後，極有希望一舉達成連續 60 秒擷取與正常 Stop/Run 循環！
