# implement_plan2.md — Stage 4 執行計畫（含 `argon_reply4` 補正）

最後更新：2026-10-29
狀態：**規劃完成，尚未修改任何程式碼**
前置文件：`plan_stage4.md`、`argon_reply4.md`

---

## 1. 目的與範圍

解決 Stage 4：DSWave 按下執行後，bulk IN `0x86` 逾時、裝置不送資料。
本計畫只改兩個檔案（`helper.c`、`winusb_shim.c`）與 `proto.h`，
不動 `setupapi` / `cfgmgr32` / `ioctl_hook`。

完成定義（DoD）：
- 第一筆 bulk 立即 `ERROR_IO_PENDING`，不再阻塞；
- 控制傳輸可與 bulk 並行；
- 擷取能拿到資料並繪出波形（或確認進入備援排查）。

---

## 2. 已驗證的關鍵事實

| 事實 | 證據 |
|---|---|
| libusb 以 OVERLAPPED 提交 bulk，接受 `TRUE` 或 `FALSE + ERROR_IO_PENDING` | `windows_winusb.c` `winusbx_submit_bulk_transfer()` L4216-4226 |
| `FALSE + ERROR_GEN_FAILURE` 會被 libusb 直接當成 submit 失敗（`LIBUSB_ERROR_IO`） | 同上 L4224-4226 |
| libusb 呼叫 `ReadPipe(..., transferred = NULL, overlapped)` | L4218（第 5 個參數是 `NULL`） |
| libusb 以 `HasOverlappedIoCompleted()` 判斷完成 | `windows_winusb.c` L777 |
| 取消/逾時走 `WinUsb_AbortPipe` | `winusbx_cancel_transfer()` |
| handle 本身 thread-safe，鎖只需保護 `handles[]` | libusb API 保證 |

**因此目前同步路徑有兩個 bug：**
1. 失敗時回 `ERROR_GEN_FAILURE`，libusb 當成 submit 失敗（不是逾時）。
2. `transferred == NULL` 時，`overlapped->InternalHigh` 永遠是 0（即使有資料）。

---

## 3. `argon_reply4` 四項補正（列為硬性需求）

### R1：不得依賴 `transferred` 指標
所有 `InternalHigh` 一律用本地 `actual`（來自 `resp.len`）填寫。
同步退回路徑也一樣。

### R2：`AbortPipe` 不得直接改 in-flight 的 `overlapped`
把請求分成兩個狀態：
- **QUEUED**（尚未被 worker 取走）：`AbortPipe` 可從佇列移除、直接完成（`STATUS_CANCELLED` + `SetEvent`）並釋放 ctx。
- **INFLIGHT**（worker 已取走、正在等 helper）：`AbortPipe` **只設 `ctx->cancelled = 1`**，並送 `REQ_ABORT_PIPE`；由 **worker 自己**在收到 `LIBUSB_ERROR_INTERRUPTED` 後設定 `STATUS_CANCELLED` 與 `SetEvent`。
  這樣 `buffer` / `overlapped` 在 worker 結束前不會被 libusb 釋放（避免 UAF）。

### R3：`abort_gen` 用遞增序號，不用清除旗標
```c
static volatile uint32_t abort_gen[MAX_HANDLES][256];
```
- `do_abort_pipe()`：`__sync_add_and_fetch(&abort_gen[id][ep], 1);`
- `do_bulk()`：進入迴圈前 `my_gen = __sync_fetch_and_add(&abort_gen[id][ep], 0);`
  迴圈中若 `abort_gen[id][ep] != my_gen` → `LIBUSB_ERROR_INTERRUPTED`。
- 新請求自然取得新 generation，不會被舊的清除動作覆蓋。

### R4：同 endpoint 的 FIFO 與部分讀取累積
- **FIFO**：每個 `(handle, endpoint)` 一把 `pthread_mutex_t ep_lock[MAX_HANDLES][256]`，
  `do_bulk()` 進入切片迴圈前鎖住，離開時解鎖。不同 endpoint 完全平行。
- **部分讀取**：
```c
uint32_t total = 0;
for (;;) {
    int xfer = 0;
    int remain = (int)(len - total);
    ret = libusb_bulk_transfer(h, ep, buf + total, remain, &xfer, slice);
    if (xfer > 0) total += (uint32_t)xfer;
    if (ret == 0 || total >= len) { ret = 0; break; }
    if (ret != LIBUSB_ERROR_TIMEOUT) break;
    if (gen changed) { ret = LIBUSB_ERROR_INTERRUPTED; break; }
    if (deadline reached) break;
}
resp->len = total;
```
- 切片長度 200 ms；無限等待時安全上限 300000 ms。

---

## 4. 設計總覽

```
WinUsb_ReadPipe(overlapped)
  ├─ ctx 狀態 = QUEUED，入佇列
  ├─ overlapped->Internal = STATUS_PENDING
  ├─ ResetEvent(hEvent)
  └─ return FALSE + ERROR_IO_PENDING
            │
            ▼
      worker pool（8–16 條）
            ├─ 狀態 → INFLIGHT
            ├─ 開自己的 TCP 連線
            ├─ REQ_BULK（獨立 socket，不佔控制連線）
            ├─ 完成：InternalHigh = actual、Internal = 對應狀態
            └─ SetEvent(hEvent)   ← 只有 worker 做
```

控制傳輸（`WinUsb_ControlTransfer`）維持在原本長連線、同步完成。

---

## 5. 實作步驟

### S1 — `proto.h`

新增：
```c
REQ_ABORT_PIPE = 7,   /* a = endpoint */
REQ_RESET_PIPE = 8,   /* a = endpoint */
```

驗收：建置通過，現有功能不受影響。

---

### S2 — `helper.c`

1. **`get_handle()`**：只鎖 `handles[]` 查表，transfer 期間不持鎖。
2. **移除 `do_control` / `do_bulk` / `do_descriptor` 內的 `pthread_mutex_lock(&usb_lock)`**。
3. **`abort_gen[MAX_HANDLES][256]`**（R3）。
4. **`ep_lock[MAX_HANDLES][256]`**（R4）：靜態初始化 `PTHREAD_MUTEX_INITIALIZER` 陣列。
5. **`do_bulk()`**：依 R4 的切片 + 累積 + generation 檢查；日誌：
   `bulk start ep=%02x len=%u` / `bulk done ep=%02x status=%s total=%u`。
6. **`do_abort_pipe()`**：`abort_gen[id][ep]++`。
7. **`do_reset_pipe()`**：`libusb_clear_halt(h, ep)`。
8. **handle refcount（建議）**：`REQ_CLOSE` 等 in-flight 歸零才真正 `libusb_close`。

驗收：
- `helper --selftest` 正常；
- Python 客戶端可同時開兩條連線，一條 bulk 等待、一條 control 立即回應；
- 送 `REQ_ABORT_PIPE` 後 bulk 在 ≤ 250 ms 內回 `LIBUSB_ERROR_INTERRUPTED`。

---

### S3 — `winusb_shim.c`

1. **狀態常數**
```c
#define STATUS_SUCCESS      0x00000000UL
#define STATUS_PENDING      0x00000103UL
#define STATUS_CANCELLED    0xC0000120UL
#define STATUS_IO_TIMEOUT   0xC00000B5UL
#define STATUS_UNSUCCESSFUL 0xC0000001UL
```

2. **bulk ctx**
```c
struct bulk_ctx {
    struct ctx *dev;
    UCHAR pipe;
    PUCHAR buffer;
    ULONG length;
    ULONG timeout;
    int is_in;
    LPOVERLAPPED overlapped;
    volatile LONG cancelled;
    volatile LONG state;   /* 0=queued 1=inflight 2=done */
};
```

3. **worker pool**：固定 8–16 條執行緒 + queue；
   每筆 worker 用 `open_helper_socket()` 開自己的連線。

4. **`WinUsb_ReadPipe` / `WritePipe`**
```c
if (overlapped) {
    overlapped->Internal = STATUS_PENDING;
    overlapped->InternalHigh = 0;
    if (overlapped->hEvent) ResetEvent(overlapped->hEvent);
    if (enqueue(ctx)) { SetLastError(ERROR_IO_PENDING); return FALSE; }
    /* 佇列滿：退回同步，但 InternalHigh 用 actual（R1） */
}
```

5. **`WinUsb_AbortPipe`**（R2）
   - QUEUED：出列、`STATUS_CANCELLED`、`SetEvent`、釋放 ctx。
   - INFLIGHT：`ctx->cancelled = 1`；同步送 `REQ_ABORT_PIPE`（控制連線）。
   - **不觸碰 in-flight 的 overlapped**。

6. **`WinUsb_ResetPipe`** → 送 `REQ_RESET_PIPE`，回傳 helper 結果。

7. **`WinUsb_GetOverlappedResult`**
```c
if (Internal == STATUS_PENDING) {
    if (!wait) { SetLastError(ERROR_IO_INCOMPLETE); return FALSE; }
    WaitForSingleObject(hEvent, INFINITE);
}
*transferred = (DWORD)InternalHigh;
switch (Internal) {
  case STATUS_SUCCESS: return TRUE;
  case STATUS_IO_TIMEOUT: SetLastError(ERROR_SEM_TIMEOUT); return FALSE;
  case STATUS_CANCELLED: SetLastError(ERROR_OPERATION_ABORTED); return FALSE;
  default: SetLastError(ERROR_GEN_FAILURE); return FALSE;
}
```

8. **worker 完成規則**
   - `resp.status == 0` → `InternalHigh = resp.len`（**不是** `*transferred`，R1）、`Internal = STATUS_SUCCESS`
   - `-7` → `STATUS_IO_TIMEOUT`
   - `-10`（INTERRUPTED）或 `ctx->cancelled` → `STATUS_CANCELLED`
   - 其他 → `STATUS_UNSUCCESSFUL`
   - 最後才 `SetEvent`；之後不得再碰 `buffer` / `overlapped`。

9. **控制傳輸**：不動；`overlapped` 立即完成。

驗收（單元）：
- 用一支測試程式對 shim 呼叫 `WinUsb_ReadPipe`，確認：
  - 立即回 `FALSE` + `ERROR_IO_PENDING`；
  - `overlapped->Internal` 最後變成 `STATUS_SUCCESS` 且 `InternalHigh = 實際長度`；
  - `AbortPipe` 後 `Internal = STATUS_CANCELLED`，且無 crash。

---

### S4 — 建置與安裝

`build.sh` 不需改（DLL 與 helper 都已包含）。
`install.sh` 重跑即可。

---

### S5 — 整合驗證（V4.1–V4.6）

| 編號 | 操作 | 通過條件 |
|---|---|---|
| V4.1 | `LIBUSB_DEBUG=4 wine DSWave.exe -l 5` | 可列出 `H0: DSCope M454`、READY |
| V4.2 | 按執行 | `winusb.log` 第一筆 bulk 立即 PENDING；同時間出現多筆 bulk 或 control |
| V4.3 | 觀察 helper | `bulk start` 等待中，`req=4` 控制仍立即回應 |
| V4.4 | 擷取 | `bulk done status=0 total=4096`，UI 出現波形 |
| V4.5 | 按停止 | in-flight 於 <1 s 結束（`STATUS_CANCELLED`），UI 不凍結 |
| V4.6 | 重跑 3 次 | helper handle 數回到 0，無殭屍 worker/連線 |

---

### S6 — 備援（僅在 V4.4 失敗時）

1. endpoint 是否應為 `0x84`（在 shim 記錄 submit 的 pipe 與次數）。
2. arm/trigger 命令是否真的送出（比對控制序列）。
3. `RAW_IO` / `MAXIMUM_TRANSFER_SIZE` pipe policy。
4. 開啟後先 `clear_halt` 全部 endpoint。

---

## 6. 風險與對策

| 風險 | 對策 |
|---|---|
| UAF（Abort 與 worker 競爭） | R2：in-flight 只標記，由 worker 完成 |
| 掉包（切片邊界） | R4：累積 `total`，從 `buf+total` 續讀 |
| 同 endpoint 亂序 | R4：`ep_lock` 保證 FIFO |
| abort 被後續請求清掉 | R3：generation counter |
| `InternalHigh` 永遠 0 | R1：不使用 `transferred` 指標 |
| worker/連線爆量 | 固定 pool 8–16，完成即釋放連線 |
| handle 提早關閉 | refcount；`REQ_CLOSE` 等歸零 |
| 佇列滿退回同步 | 記錄警告；必要時加大 pool |

---

## 7. 工時與順序

| 步驟 | 內容 | 估時 |
|---|---|---|
| S1 | proto.h | 0.1 天 |
| S2 | helper.c（鎖、generation、切片、abort/reset） | 0.5 天 |
| S2-驗 | 自測 + Abort 測試 | 0.2 天 |
| S3 | winusb_shim.c（async、pool、abort、狀態） | 0.5 天 |
| S4–S5 | 重裝 + V4.1–V4.6 | 0.3–0.5 天 |
| 合計 | | **約 1.5 天** |

執行順序：**S1 → S2 → S2 驗 → S3 → S4 → S5 →（必要時 S6）**。

---

## 8. 完成檢查清單（DoD）

- [ ] `proto.h` 有 `REQ_ABORT_PIPE` / `REQ_RESET_PIPE`
- [ ] helper 的 transfer 不再持 `usb_lock`
- [ ] `abort_gen` / `ep_lock` / 切片累積已實作
- [ ] `WinUsb_ReadPipe` 對 overlapped 立即回 `ERROR_IO_PENDING`
- [ ] `InternalHigh` 一律用 `resp.len`（不依賴 `transferred`）
- [ ] `AbortPipe` 不碰 in-flight 的 overlapped
- [ ] V4.1–V4.6 全數通過（或已進入 S6 並有結論）
- [ ] `progress.md` 更新結果

---

## 9. 目前狀態

- 本文件為計畫；**尚未修改任何程式碼**
- 三顆墊片仍安裝於 `~/.wine-dswave`，helper 執行中
- 若要開始實作：依 S1 → S5 執行，每步完成後建置並跑對應驗收
