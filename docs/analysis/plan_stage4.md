# Stage 4 實作計畫：非同步 Bulk 傳輸與可中止的擷取

最後更新：2026-10-29
狀態：**僅規劃，尚未執行任何程式修改**
依據：`argon_reply3.md`、libusb `windows_winusb.c`（master）、現有 `winusb.log` / `dswave-helper.log`

---

## 一、現況

已通過：

| 階段 | 結果 |
|---|---|
| PnP / USB 拓撲 | `(bus: 1, addr: 6, depth: 1, port: 6)` |
| Descriptor / 字串 | `DreamSourceLab` / `USB-based DSL Instrument v3` 讀取成功 |
| 硬體初始化 | `INIT → OPEN → READY`、`Fpga is already done`、`MSO ZERO loaded` |
| UI | 出現 `H0: DSCope M454` 分頁 |

未通過（Stage 4）：按下執行後，bulk IN `0x86` 讀 4096 bytes
先回 `LIBUSB_ERROR_IO`、`clear_halt` 後重試 60 秒 `LIBUSB_ERROR_TIMEOUT`，
最終 `HAL_EVENT_DEVICE_ERROR`。

---

## 二、根因覆核（已對照 libusb 原始碼）

### 根因 1：同步 submit 讓應用無法完成「掛 ring → 啟動」的順序

libusb `winusbx_submit_bulk_transfer()`（L4216-4226）：

```c
ret = WinUSBX[sub_api].ReadPipe(winusb_handle, transfer->endpoint,
                                transfer->buffer, transfer->length, NULL, overlapped);
...
if (!ret && GetLastError() != ERROR_IO_PENDING) { ... return LIBUSB_ERROR_IO; }
return LIBUSB_SUCCESS;
```

- libusb **接受**「立即回 TRUE」或「FALSE + ERROR_IO_PENDING」。
- 但我們的 `WinUsb_ReadPipe` 在 `submit` 內同步阻塞 60 秒，
  應用因此無法提交後續 transfer，也無法在 submit 之後下達「啟動/觸發」命令
  → 裝置永遠不會送資料。
- 這是 Stage 4 的主因。

### 根因 2：全域鎖被 bulk 佔住

| 位置 | 問題 |
|---|---|
| `winusb_shim.c` `bulk_transfer()` | `EnterCriticalSection(&lock)` 包住整個 60 秒 RPC；其他執行緒的 `ControlTransfer` 全卡住 |
| `helper.c` `do_bulk()` | `pthread_mutex_lock(&usb_lock)` 包住 `libusb_bulk_transfer()`；控制傳輸無法穿插 |

libusb handle 本身 thread-safe，鎖只需要保護 `handles[]` 查表。

### 根因 3：`AbortPipe` / `ResetPipe` 是空殼，逾時語意不符

- libusb 的逾時/取消機制是：使用者層計時 → `libusb_cancel_transfer()`
  → `winusbx_cancel_transfer()` → `WinUsb_AbortPipe()`。
- 我們的 `WinUsb_AbortPipe` / `WinUsb_ResetPipe` 只 `return TRUE`，
  沒有任何實際作用。
- `PIPE_TRANSFER_TIMEOUT = 0` 在 WinUSB 代表「無限等待」，
  我們卻把它 clamp 成 60 秒，且無法被 abort 打斷。

---

## 三、對 `argon_reply3.md` 的修正

| 項目 | argon_reply3 的說法 | 修正 |
|---|---|---|
| 執行緒模型 | 每個 transfer 開一條執行緒 + 一條 TCP | 改為 **worker pool（8–16）＋ 佇列**，避免連續擷取時執行緒爆量 |
| timeout 欄位 | 用 `b` / `c` 拆分 32-bit | `shim_req.b` 已是 32-bit，直接放 timeout 即可 |
| 完成路徑 | 強調 `WinUsb_GetOverlappedResult` | libusb 實際用 `HasOverlappedIoCompleted()` 讀 `Internal` / `InternalHigh`；worker 必須把這兩個欄位與 `hEvent` 設對，`GetOverlappedResult` 只是補完整性 |
| handle 生命週期 | 未提 | bulk 連線引用 control 連線開的 handle；control 連線斷開時 `close_owned` 會關掉 handle，in-flight bulk 會失敗。需要 refcount 或明確記錄 |
| 逾時語意 | 建議 60 秒 clamp | 由 libusb 的 `AbortPipe` 主導，helper 端只保留安全上限（例如 5 分鐘），並用短切片輪詢 |

---

## 四、設計

```
DSWave (libusb)
  └─ WinUsb_ReadPipe(overlapped)
       ├─ overlapped->Internal = STATUS_PENDING
       ├─ 丟進 worker pool 佇列
       └─ return FALSE, ERROR_IO_PENDING
                 │
                 ▼
        worker thread（每筆一個 TCP 連線）
                 │ REQ_BULK
                 ▼
        helper：每 200 ms 切片做 libusb_bulk_transfer
                 ├─ 有資料 → 回 status=0, len
                 ├─ abort flag → LIBUSB_ERROR_INTERRUPTED
                 └─ deadline → LIBUSB_ERROR_TIMEOUT
```

控制傳輸維持在原本的長連線、同步完成（每次 <1 ms），不再被 bulk 阻塞。

---

## 五、檔案變更清單

### 5.1 `src/proto.h`

新增請求類型：

```c
enum {
    REQ_LIST = 1,
    REQ_OPEN,
    REQ_CLOSE,
    REQ_CONTROL,
    REQ_BULK,
    REQ_DESCRIPTOR,
    REQ_ABORT_PIPE,   /* a = endpoint */
    REQ_RESET_PIPE,   /* a = endpoint */
};
```

### 5.2 `src/helper.c`

1. **鎖範圍縮小**

```c
static libusb_device_handle *get_handle(uint32_t id)
{
    libusb_device_handle *h = NULL;
    pthread_mutex_lock(&usb_lock);
    if (id >= 1 && id < MAX_HANDLES)
        h = handles[id];
    pthread_mutex_unlock(&usb_lock);
    return h;
}
```

- `do_control()` / `do_bulk()` / `do_descriptor()` 取得 handle 後**不再持鎖**做 transfer。
- `do_open()` / `do_close()` 仍持鎖（保護 `handles[]`）。

2. **per-handle abort flag**

```c
static volatile int abort_flags[MAX_HANDLES];
```

- `do_abort_pipe()`：`abort_flags[id] = 1`。
- `do_bulk()`：每次 transfer 前清除 flag；以 200 ms 切片迴圈：

```c
for (;;) {
    remaining = deadline - now();
    if (remaining <= 0) { ret = LIBUSB_ERROR_TIMEOUT; break; }
    slice = min(200, remaining);
    if (abort_flags[id]) { ret = LIBUSB_ERROR_INTERRUPTED; break; }
    ret = libusb_bulk_transfer(h, ep, buf, len, &transferred, slice);
    if (ret == 0) break;
    if (ret != LIBUSB_ERROR_TIMEOUT) break;
}
```

- timeout 0（無限）→ 內部上限 300000 ms（5 分鐘）。

3. **`do_reset_pipe()`**：`libusb_clear_halt(h, ep)`。

4. **日誌**：
   - `bulk start ep=%02x len=%u timeout=%u`
   - `bulk done ep=%02x status=%s len=%u`
   - 控制傳輸維持現有的一行摘要。

5. **handle refcount（可選但建議）**
   - `struct handle_ref { libusb_device_handle *h; int refs; }`
   - 每個 in-flight bulk +1，完成 -1；`REQ_CLOSE` 等到 refs=0 才真正 close。

### 5.3 `src/winusb_shim.c`

1. **async 狀態常數**

```c
#define STATUS_SUCCESS       0x00000000UL
#define STATUS_PENDING       0x00000103UL
#define STATUS_CANCELLED     0xC0000120UL
#define STATUS_IO_TIMEOUT    0xC00000B5UL
#define STATUS_UNSUCCESSFUL  0xC0000001UL
```

2. **transfer context + worker pool**

```c
struct bulk_ctx {
    struct ctx *dev;
    UCHAR pipe;
    PUCHAR buffer;
    ULONG length;
    ULONG timeout;
    int is_in;
    LPOVERLAPPED overlapped;
};
```

- 建立固定 8–16 條 worker thread + 一個簡單的 queue（`CRITICAL_SECTION` + condition/event）。
- 每筆 worker 使用**自己的 TCP 連線**（`open_helper_socket()`），送出 `REQ_BULK`，
  完成後設定 `overlapped` 並 `SetEvent`。

3. **`WinUsb_ReadPipe` / `WinUsb_WritePipe`**

```c
if (overlapped) {
    overlapped->Internal = STATUS_PENDING;
    overlapped->InternalHigh = 0;
    if (overlapped->hEvent) ResetEvent(overlapped->hEvent);
    if (enqueue(ctx)) { SetLastError(ERROR_IO_PENDING); return FALSE; }
    /* queue 滿時退回同步 */
}
```

4. **`WinUsb_AbortPipe`**

```c
for each pending ctx on (dev, pipe):
    ctx->cancelled = 1;
    ctx->overlapped->Internal = STATUS_CANCELLED;
    ctx->overlapped->InternalHigh = 0;
    SetEvent(ctx->overlapped->hEvent);
send REQ_ABORT_PIPE (background, 不阻塞)
return TRUE;
```

5. **`WinUsb_ResetPipe`** → 送 `REQ_RESET_PIPE`。

6. **`WinUsb_GetOverlappedResult`**

```c
if (!overlapped) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
if (overlapped->Internal == STATUS_PENDING) {
    if (!wait) { SetLastError(ERROR_IO_INCOMPLETE); return FALSE; }
    if (overlapped->hEvent) WaitForSingleObject(overlapped->hEvent, INFINITE);
}
*transferred = (DWORD)overlapped->InternalHigh;
switch (overlapped->Internal) {
    case STATUS_SUCCESS: return TRUE;
    case STATUS_IO_TIMEOUT: SetLastError(ERROR_SEM_TIMEOUT); return FALSE;
    case STATUS_CANCELLED: SetLastError(ERROR_OPERATION_ABORTED); return FALSE;
    default: SetLastError(ERROR_GEN_FAILURE); return FALSE;
}
```

7. **控制傳輸**：維持同步、使用原本長連線；`overlapped` 立即完成（現行行為）。
   `PIPE_TRANSFER_TIMEOUT` 仍記錄，但 async bulk 的逾時交由 libusb 的 `AbortPipe`。

8. **移除 `bulk_transfer()` 內的 `EnterCriticalSection(&lock)`**：
   控制 socket 專用一把鎖；bulk 走獨立連線，不需要共用鎖。

---

## 六、驗證矩陣

| 編號 | 操作 | 通過條件 |
|---|---|---|
| V4.1 | 啟動 DSWave（`LIBUSB_DEBUG=4 -l 5`） | 仍可列出 `H0: DSCope M454`、READY |
| V4.2 | 按執行 | `winusb.log`：第一筆 bulk 立即 `ERROR_IO_PENDING`；同一時間出現多筆 bulk 或 control |
| V4.3 | 觀察 helper log | `bulk start` 掛著的同時，`req=4`（control）立即 `status=0` |
| V4.4 | 擷取 | `bulk done status=0 len=4096`，UI 出現波形（非空白） |
| V4.5 | 按停止 | in-flight transfer 在 <1 秒內結束（`STATUS_CANCELLED`），UI 不卡住 |
| V4.6 | 重複執行 3 次 | 無 handle 洩漏（helper 的 handle 數回到 0） |

若 V4.4 仍無資料，進入備援排查（下節）。

---

## 七、備援排查（若仍無資料）

1. **endpoint 是否應為 `0x84`**
   - 在 shim 記錄「哪個 pipe 被 submit」與次數；
   - 必要時讓 helper 提供手動測試：claim 後同時掛 0x86 / 0x84 讀取，看哪個有資料。
2. **啟動/觸發命令是否真的送出**
   - 目前控制命令都走 `req=0xb0`；確認 submit 不再阻塞後，
     `Sync:Word` 之後是否有新的 `0xb0/0xb1` 命令。
3. **pipe policy**
   - 檢查是否需要 `RAW_IO`（0x07）或 `MAXIMUM_TRANSFER_SIZE`（0x08）。
4. **硬體狀態**
   - 若 `0x86` 持續 stall，嘗試在開啟後先 `libusb_clear_halt` 全部 endpoint。

---

## 八、風險與對策

| 風險 | 對策 |
|---|---|
| 執行緒/連線過多 | worker pool 上限 8–16；每筆完成即釋放連線 |
| abort 與 close 競爭 | handle refcount；`REQ_CLOSE` 等 refs 歸零 |
| 狀態碼對應錯誤 | 只用 `Internal` / `InternalHigh` 標準語意，log 驗證 |
| 佇列滿導致退回同步 | 記錄警告；必要時加大 pool 或回壓 |
| 無限等待造成殭屍 worker | helper 端 5 分鐘上限；abort 每 200 ms 檢查 |

---

## 九、預估工時

| 工作 | 估時 |
|---|---|
| helper.c（鎖範圍、abort/reset、切片、日誌） | 0.5 天 |
| winusb_shim.c（async submit、worker pool、abort/reset、狀態） | 0.5 天 |
| 驗證與調校 | 0.3–0.5 天 |
| **合計** | **約 1–1.5 天** |

---

## 十、執行順序（待確認後才動手）

1. `proto.h`：新增 `REQ_ABORT_PIPE` / `REQ_RESET_PIPE`
2. `helper.c`：鎖範圍、切片 bulk、abort/reset、日誌
3. 建置 + `helper --selftest` + 用 Python 客戶端測試 abort
4. `winusb_shim.c`：async submit + worker pool + abort/reset + 狀態
5. `install.sh` 重裝、`LIBUSB_DEBUG=4` 跑 V4.1–V4.6
6. 若 V4.4 失敗 → 依第七節排查

---

## 附註：目前程式狀態

- 三顆墊片已安裝於 `~/.wine-dswave`（`setupapi` / `cfgmgr32` / `winusb` = native）
- helper 執行中
- 本文件僅為計畫；**尚未修改任何程式碼**
