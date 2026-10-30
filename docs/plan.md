# DSWave（Wine + WinUSB 墊片）資料流穩定性改進計畫

版本：**v2（2026-10-30）** — 併入 `argon_reply9.md` 審查意見（3 項實作補強）
參考：`argon_reply8.md`、`argon_reply9.md`、`progress.md`、`/tmp/dswave-helper.log`、`fpga1.pcap`

---

## 0. v2 變更摘要

| # | 變更 | 來源 |
|---|---|---|
| 1 | **新增 `hook_CancelIoEx`**（P1-1 必要條件）：App 的 `libusb_cancel_transfer()` 會先呼叫 `CancelIoEx`，因假檔案回 `ERROR_NOT_FOUND (0x490)` 而**跳過 `WinUsb_AbortPipe`** | argon_reply9 §2.1 |
| 2 | **P0-2 取消生命週期**：`libusb_cancel_transfer()` 後必須留在 event loop 等 callback，設 `cancelling` 旗標，嚴禁提前 free（UAF） | argon_reply9 §2.2 |
| 3 | **P1-2 狀態碼對照已反組譯確認**：App 用 `KERNEL32!GetOverlappedResult`；`STATUS_CANCELLED → ERROR_OPERATION_ABORTED(995)` → `LIBUSB_TRANSFER_CANCELLED`；真實 I/O 錯誤用 `STATUS_DEVICE_DATA_ERROR` → `ERROR_CRC(23)` → `LIBUSB_TRANSFER_ERROR` | argon_reply9 §2.3 |

---

## 1. 目標

在 **不重新編譯 Wine、只修改 prefix／墊片** 的前提下：

1. 連續擷取 **60 秒以上**不中斷（目前 0.3–1.1 秒就 `-EPROTO`）
2. 按 Stop 後能再次 Run（目前第二次 Run 必跳「USB 不穩定」）
3. Stop／異常中斷後**不需重刷 FPGA**即可恢復

---

## 2. 已知事實（證據）

| 觀察 | 來源 |
|---|---|
| 不經 App 直接讀 `0x86`，405,504 bytes @20 MB/s → **170 MB / 34 影格 / 0 錯誤** | `/tmp/dswbulk` |
| Windows：`0x86` IN 405,504 ×133、間隔 ~22 ms（18.6 MB/s）；停止時 pending read 以 `0xC0010000` + len 0 結束 | `fpga1.pcap` |
| Wine 下 0.3–1.1 秒後 libusb 回 **-EPROTO（urb status=-71）** | helper log |
| 中斷後裝置卡死：只回 4096 header、無 frame 資料 | helper log |
| 熱插拔執行緒每 ~100 ms 觸發 `REQ_LIST`（本次 log **1,627 次**／130,913 行） | helper log |
| Stop 後 App 只送到 `01 00 00 → 02 08 00 → 01 00 00`，**缺 `02 04 00`** | helper log |
| `0xC0010000` 被 App 解讀成 `LIBUSB_SUCCESS / LIBUSB_TRANSFER_COMPLETED; length 0` | app log |

### 2.1 決定性時間軸（argon_reply9 §1，已用 log 覆核）

```
12.3165  seq=1（5 MB）開始，libusb 提交 811,008-byte bulk IN 到 0x86
12.3178  熱插拔執行緒 libusb_open(8.15) → 送出 EP0 GET_DESCRIPTOR
         （bulk 啟動後僅 1.35 ms）
13.3178  該筆 EP0 GET_DESCRIPTOR 超時被取消（等了 1000 ms）
14.3165  0x86 bulk 在 335,872 bytes 處被 2000 ms 切片超時取消
14.3577  EP0 恢復，GET_DESCRIPTOR 回 18 bytes
14.3578  下一次 0x86 submit → urb status=-71（EPROTO）
```

→ 裝置在 EP0 被要求 descriptor 的瞬間同時凍結 EP0 與 `0x86`；**EP0 流量與 bulk 串流互斥**是本計畫最重要的假設。

---

## 3. 根因假設（依優先序）

**H1（P0）— 熱插拔輪詢在高速串流期間打斷 FX3**
`REQ_LIST → do_list() → read_descriptors()` 每次對同一裝置開新 fd、送 3 筆 EP0 `GET_DESCRIPTOR`、再 `libusb_close()`。
在 `0x86` SuperSpeed bulk 串流期間每秒數十次 EP0，會讓 FX3 凍結（見 §2.1）→ `-EPROTO`。
這正好解釋 `dswbulk`（無此輪詢）能跑 170 MB 零錯誤。

**H2（P0）— 200 ms 切片造成 URB Discard / Resubmit**
`do_bulk()` 以 200 ms 為單位取消並重送 URB；FX3 的 burst 端點被反覆 Discard 會直接觸發 `-EPROTO` 或卡住 FIFO。

**H3（P1）— Stop 握手被腰斬、`02 04 00` 未送出**
Windows Stop 序列：`01 00 00 → 02 08 00 → (0x84 讀 1B)×3 → 01 00 00 → 02 04 00`。
App 的停止流程會**阻塞等待 `0x86` transfer 退回**，才送第二個 `01 00 00` 與 `02 04 00`。
我們在 `01 00 00` 就自動 abort，且因 EPROTO 走錯誤分支 → `02 04 00` 永遠不會送出 → FPGA 擷取 FIFO 卡死。

**H4（P1）— 狀態碼型別錯誤**
`0xC0010000` 是 `USBD_STATUS_CANCELED`，不是 `NTSTATUS`；`OVERLAPPED.Internal` 必須放 `STATUS_CANCELLED (0xC0000120)`，否則 libusb 的 IOCP 完成路徑會誤判。

**H5（P2）— FPGA bitstream 上傳在 Wine 下失敗**
App 仍會把 bitstream 經 `0x02` bulk OUT 送出（helper 收得到）；可先攔截存檔，之後不必依賴 Windows VM。

---

## 4. 工作項目

### P0-1　移除 `REQ_LIST` 的匯流排副作用

- `src/helper.c`
  - `read_descriptors()`：**禁止** `libusb_open()` / `libusb_control_transfer()` / `libusb_close()`。
  - 以 `(bus, addr, vid, pid)` 為 key 快取 descriptor；首次或位址變更時改讀 `/sys/bus/usb/devices/<bus>-<port>/descriptors`。
  - 若 `handles[1]` 已開啟，直接回傳 `do_open()` 時快取的資料。
- `src/setupapi_shim.c`
  - `refresh_dev_identity()` 加 **2 秒節流**（`GetTickCount()`），節流期間沿用 `cur_dev`，不發 IPC。
  - 位址變更（重新列舉）時必須立即失效快取並更新 `WM_DEVICECHANGE`。
- 驗收：擷取期間 `req=1` 次數 = **0**；Run 30 秒無 `-EPROTO`。
- 回滾：`DSW_LIST_LIVE=1` 恢復舊行為。

### P0-2　廢除 200 ms 切片（非同步長傳輸 + 正確取消生命週期）

- `src/helper.c` `do_bulk()`
  1. `libusb_fill_bulk_transfer()` + `libusb_submit_transfer()`，timeout 用 `req->b`（App 給 300,000 ms）。
  2. 迴圈 `libusb_handle_events_timeout_completed(ctx, 50 ms)`，每輪檢查 `abort_gen`。
  3. **取消流程**：
     - 偵測到 `abort_gen` 變更且 `!cancelling` → `libusb_cancel_transfer(xfer)`，設 `cancelling = 1`；
     - **繼續跑 event loop 直到 callback 觸發**（`done = 1`）才可 `libusb_free_transfer()` / `free(buf)`；
     - 絕不可在 cancel 後直接 break → UAF。
  4. callback 內記錄 `actual_length` / `status`，短封包（short packet）視為本輪完成。
  5. `fill` 邏輯保留：以 `chunk` 累積到 `req->len` 再回傳；`chunk` 預設 **405,504**（對齊 Windows）。
  6. 邊界：`5,005,312 = 12 × 405,504 + 139,264`，最後一筆本來就會是短讀，屬正常。
- 實驗備案：若 5 MB 組裝仍有問題，改 `DSW_FILL=0`（每 405,504 直接回傳；App 的 callback 只要求 `status == COMPLETED && actual > 0`）。
- 驗收：libusb debug log 正常串流期間 **0 次** `libusb_cancel_transfer`；Run 60 秒波形不中斷。
- 回滾：`DSW_SLICE_MS` 保留作退路。

### P1-1　Stop 序列不再提早 abort + **掛鉤 `CancelIoEx`（必要）**

**為什麼需要（argon_reply9 §2.1）**
App 的停止流程：
1. 送 `01 00 00`、`02 08 00`、讀 `0x84`
2. 對飛行中的 `0x86` 呼叫 `libusb_cancel_transfer()`
3. **阻塞等 transfer 退回**，才送 `01 00 00` 與 `02 04 00`

但 libusb 的 `windows_cancel_transfer` 會先呼叫 `KERNEL32!CancelIoEx(hFile, ov)`；我們的 `hFile` 是假檔案，Wine 回 `FALSE + ERROR_NOT_FOUND (0x490)`，App 因此**直接回傳 `LIBUSB_ERROR_NOT_FOUND`，不會 fallback 到 `WinUsb_AbortPipe`**。
→ 若不修，P0-2 + P1-1 會讓 Stop 卡 300 秒。

**做法**
- `src/ioctl_hook.c`：在 IAT hook 安裝時一併掛 `KERNEL32!CancelIoEx`：

```c
typedef BOOL (WINAPI *cancel_io_ex_t)(HANDLE, LPOVERLAPPED);
static cancel_io_ex_t real_cancel_io_ex;

static BOOL WINAPI hook_CancelIoEx(HANDLE h, LPOVERLAPPED ov)
{
    if (shim_owns_handle(h) && getenv("DSW_CANCELIO_HOOK") /* 預設開啟 */)
    {
        /* 不可回 ERROR_NOT_FOUND(0x490)，否則 libusb 不會 fallback */
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    return real_cancel_io_ex ? real_cancel_io_ex(h, ov) : FALSE;
}
```

- `src/helper.c` `do_control()`
  - 移除收到 `01 00 00` 時 `abort_gen++` 的行為。
  - 保留 `02 04 00` 時 abort 作為雙重保險。
- 驗收：Stop 時 helper 依序看到完整五步；`REQ_ABORT_PIPE` 出現在 `02 08 00` 之後、`02 04 00` 之前。

### P1-2　狀態碼改回 NTSTATUS（已反組譯確認）

App 使用 `KERNEL32!GetOverlappedResult`；Wine 會 `SetLastError(RtlNtStatusToDosError(ov->Internal))`。

| 情境 | `ov->Internal` | Wine 轉換 | App 結果 |
|---|---|---|---|
| 取消（AbortPipe / abort_gen） | `STATUS_CANCELLED 0xC0000120` | `ERROR_OPERATION_ABORTED 995` | `LIBUSB_TRANSFER_CANCELLED (3)` |
| 真實 I/O 錯誤 | `STATUS_DEVICE_DATA_ERROR 0xC000009C` | `ERROR_CRC 23` | `LIBUSB_TRANSFER_ERROR (1)` |
| 其他失敗 | `STATUS_UNSUCCESSFUL 0xC0000001` | `ERROR_GEN_FAILURE 31` | `LIBUSB_TRANSFER_STALL (4)` |

- `src/winusb_shim.c` `run_bulk_worker()`：刪除 `0xC0010000`；取消 → `STATUS_CANCELLED`；EPROTO → `STATUS_DEVICE_DATA_ERROR`。
- `WinUsb_GetOverlappedResult()` 同步補上這兩個 case。
- 驗收：App log 顯示 `LIBUSB_TRANSFER_CANCELLED`；不再出現 `COMPLETED; length 0`。

### P1-3　軟重置（免重刷 FPGA）

- `src/helper.c` 新增 `dsw_hw_soft_reset(h)`：
  1. `b0 01 00 00`
  2. `b0 02 08 00`
  3. `0x84` 讀 1 byte ×3
  4. `b0 01 00 00`
  5. `b0 02 04 00`
  6. `libusb_clear_halt(0x86)` + 讀一次 405,504 丟棄
- 呼叫時機：`do_close()`、`do_open()` 失敗恢復、偵測到 `-EPROTO` 後。
- 驗收：卡死狀態下重開 App 即可恢復（不需 VM／插拔）。

### P2-1　攔截 FPGA bitstream（脫離 Windows VM）

- `do_bulk()`：`!is_in && ep == 0x02 && len > 100000` → `fwrite` 到 `/tmp/dswave-fpga.bin`（只寫第一次）。
- 驗收：冷啟動後檔案 466,408 bytes。

### P2-2　量測與比對

- `bulk done` 記錄：耗時、URB 取消次數、EP0 控制次數。
- 重錄 Windows pcap（`-s 1000000`，60 秒長跑），確認 Windows 是否也會在 ~95 MB 後停止。

---

## 5. 執行順序與驗證

1. **P0-1 + P0-2**（同輪修改、一起測）
   - Run 60 秒：波形持續、無 EPROTO、libusb 無 cancel
2. **P1-1（含 `hook_CancelIoEx`）+ P1-2**
   - Stop → 五步序列完整 → 再 Run 成功
3. **P1-3**
   - 擷取中 `kill -9` App → 重開 → 可直接 Run
4. **P2-1 / P2-2**
   - 冷啟動攔截 bitstream；Windows 長跑比對

回歸基準：READY < 5 秒、無 assert、D0/D1/D2 方波正常。

---

## 6. 風險與回滾

| 變更 | 風險 | 回滾 |
|---|---|---|
| REQ_LIST 快取 | 重新列舉時回報舊資料 | bus:addr 比對即失效；`DSW_LIST_LIVE=1` |
| 移除切片 | Stop 若無 AbortPipe 會等 300 秒 | `hook_CancelIoEx` + `02 04 00` 保底；`DSW_SLICE_MS` 退路 |
| `CancelIoEx` hook | 影響其他檔案 I/O | 只對墊片擁有的 handle 生效；`DSW_CANCELIO_HOOK=0` 關閉 |
| 狀態碼改 NTSTATUS | App 錯誤分類改變 | 依 §4 P1-2 對照表驗證；必要時加開關 |
| 軟重置 | 正常關閉多送命令 | 僅 close／錯誤恢復時執行 |

---

## 7. 目前裝置狀態／測試前置

- `Bus 008 Device 015`；最後一次測試後處於**卡死狀態**。
- 需先重新設定 FPGA（Windows VM）或重新插拔，才能進行 P0 驗證。
- 冷啟動時可順便完成 P2-1（bitstream 攔截）。

---

## 8. 重啟指令

```bash
pkill -9 -x DSWave.exe
WINEPREFIX=~/.wine-dswave wineserver -k
pkill -x helper

cd ~/Documents/DreamSourceLab/wine-winusb-shim
DSW_DEBUG=1 DSW_MBPS=20 setsid nohup ./build/helper >/tmp/dswave-helper.log 2>&1 </dev/null &

WINEPREFIX=~/.wine-dswave DISPLAY=:0 \
  XAUTHORITY=/run/user/1000/.mutter-Xwaylandauth.HGVCY3 \
  wine "C:\\Program Files\\DSWave\\DSWave.exe" -l 5
```
