# `progress.md` 進度審查與 Stage 4（擷取逾時 `0x86`）下一步建議 (`argon_reply3.md`)

## 一、目前進度確認（Stages 1 ～ 3 全面打通！）

審查 [`progress.md`](../progress.md#L215-L249)、[`winusb.log`]($WINEPREFIX/drive_c/ProgramData/DSWaveShim/winusb.log) 與 [`dswave-helper.log`](file:///tmp/dswave-helper.log) 後，確認先前的假 PnP 裝置樹與 Descriptor 修復已完美生效：

1. **Stage 1（PnP 與 USB 拓撲枚舉）成功**：
   - `cfgmgr32.dll` + `setupapi.dll` 三節點表（`ROOT_HUB20` $\rightarrow$ `VID_1D6B&PID_0002` $\rightarrow$ `VID_2A0E&PID_0080`）與 `DeviceIoControl` IAT hook 成功讓 `libusb_get_device_list()` 建立完整裝置拓撲 `(bus: 1, addr: 6, depth: 1, port: 6)`。
2. **Stage 2（Descriptor 快取與語言字串讀取）成功**：
   - `REQ_GET_DESCRIPTOR` 正確回傳 18-byte Device Descriptor 與 9/32-byte Config Descriptor。
   - String Descriptor `0x00` (`LangID = 0x0409`) 通過驗證，產品名稱 `DSCope M454` 與序號成功解析。
3. **Stage 3（硬體初始化、FPGA 與校正參數載入）成功**：
   - 裝置狀態順利從 `INIT -> OPEN -> READY` 轉換。
   - FPGA 狀態確認完成（`Fpga is already done (DSCope M454)`），且透過數百筆 `req=0xb0` 控制傳輸完成硬體校正表寫入（`MSO ZERO: Zero info loaded successfully`）。
   - GUI 已成功切入硬體模式並顯示 **`H0: DSCope M454`** 分頁。

---

## 二、Stage 4 卡住點根因診斷：為何 `Bulk pipe=86 len=4096` 會卡死 60 秒並 Timeout？

從 [`winusb.log`]($WINEPREFIX/drive_c/ProgramData/DSWaveShim/winusb.log#L2612-L2618) 可以看到關鍵序列：

```text
2612: ControlTransfer OUT rt=40 req=b0 value=0000 index=0000 len=3 data=21 e8 00
2614: ControlTransfer OUT rt=40 req=b0 value=0000 index=0000 len=3 data=00 01 00
2616: ControlTransfer OUT rt=40 req=b0 value=0000 index=0000 len=3 data=f9 00 00
2618: Bulk pipe=86 len=4096 timeout=60000
```

接著整整 **60 秒內沒有任何新的日誌**，直到 Linux 端 `libusb_bulk_transfer` 回傳 `LIBUSB_ERROR_TIMEOUT (-7)`，觸發 `HAL_EVENT_DEVICE_ERROR`。

比對 [`winusb_shim.c`](../../wine-winusb-shim/src/winusb_shim.c) 與 [`helper.c`](../../wine-winusb-shim/src/helper.c) 的實作，這裡同時存在 **三個致命的架構阻塞問題**：

### 根因 1：`WinUsb_ReadPipe` 違反 `OVERLAPPED` 非同步語意（把 `libusb_submit_transfer` 變成同步阻塞 60 秒）

- **DSWave / `libusb` 的運作方式**：
  DSWave 擷取波形時使用的是 `libusb` **非同步 API**（`libusb_submit_transfer()` $\rightarrow$ `winusbx_submit_bulk_transfer()`）。在 Windows 上，`libusb` 呼叫 `WinUsb_ReadPipe(..., overlapped)` 時**總是傳入非 NULL 的 `LPOVERLAPPED`**，並預期 `WinUsb_ReadPipe` **立即回傳 `FALSE` 且 `GetLastError() == ERROR_IO_PENDING`**。
- **為什麼同步阻塞會導致硬體沒有資料**：
  示波器軟體的標準擷取流程是：
  1. 先迴圈呼叫多次 `libusb_submit_transfer()`，在 Endpoint `0x86` 預先掛載一組（Ring Buffer，例如 16～32 個）非同步 Bulk IN 接收佇列；
  2. **在 `libusb_submit_transfer()` 返回後**，才下達最後的「啟動擷取 / 觸發（Arm Trigger）」控制指令，或進入 `libusb_handle_events()` 迴圈搭配狀態輪詢（透過 `req=0xb0` / `0xb1` / `0xb2` 讀寫 FPGA 狀態暫存器）。
- **目前 Shim 的問題**：
  在 [`winusb_shim.c`](../../wine-winusb-shim/src/winusb_shim.c#L460-L515) 的 `bulk_transfer()` 中，即使 `overlapped != NULL`，`WinUsb_ReadPipe` 依然在當前執行緒**同步阻塞等待 TCP 回應長達 60 秒**！這導致 DSWave 在提交**第一個** Bulk IN transfer 時整個執行緒就被卡死，既無法提交後續的 transfer ring，也無法執行後續的啟動/輪詢控制指令。

### 根因 2：`winusb_shim.c` 與 `helper.c` 在 60 秒 Bulk Read 期間全程霸佔全域鎖（Global Lock Deadlock）

即便 DSWave 是在獨立的擷取執行緒提交 Bulk Read，並由主執行緒或控制執行緒發送 `WinUsb_ControlTransfer`（啟動觸發、輪詢狀態或停止擷取），目前的實作也會發生 **跨執行緒死結（Deadlock）**：

1. **Wine 端 [`winusb_shim.c`](../../wine-winusb-shim/src/winusb_shim.c#L477-L497)**：
   `bulk_transfer()` 呼叫 `EnterCriticalSection(&lock)` 鎖住唯一的 TCP `sock` 長達 60 秒。期間任何其他執行緒呼叫 `WinUsb_ControlTransfer()` 都會卡死在 `EnterCriticalSection(&lock)`。
2. **Linux 端 [`helper.c`](../../wine-winusb-shim/src/helper.c#L367-L379)**：
   `do_bulk()` 呼叫 `pthread_mutex_lock(&usb_lock)` 並**抱著鎖呼叫 `libusb_bulk_transfer(..., 60000)`**！
   在 `libusb-1.0` 中，同一個 `libusb_device_handle` 本身就是 **Thread-Safe** 的，完全支援一個執行緒在 EP `0x86` 等待 Bulk IN 的同時，另一個執行緒在 EP `0x00` 發送 Control Transfer。`usb_lock` 只需要保護 `handles[]` 陣列的查詢與開關，**絕對不能**包住 `libusb_bulk_transfer()` 與 `libusb_control_transfer()`！

### 根因 3：`PIPE_TRANSFER_TIMEOUT = 0` 被轉成 60 秒，且 `WinUsb_AbortPipe` / `WinUsb_ResetPipe` 是空殼（Stub）

- 在 [`winusb.log`]($WINEPREFIX/drive_c/ProgramData/DSWaveShim/winusb.log#L2627) 中可以看到：
  `libusb` 初始化 Endpoint 時呼叫 `WinUsb_SetPipePolicy(PIPE_TRANSFER_TIMEOUT, 0)`（0 在 WinUSB 代表無限等待，因為 `libusb` 會在 User Space 用自己的 Timer 與 `WinUsb_AbortPipe` 管理逾時與取消）。
- 目前 [`WinUsb_AbortPipe()`](../../wine-winusb-shim/src/winusb_shim.c#L597-L601) 與 [`WinUsb_ResetPipe()`](../../wine-winusb-shim/src/winusb_shim.c#L591-L595) 只是直接 `return TRUE;`，沒有通知 `helper` 取消或重置管道。

---

## 三、下一步具體修改方案（實作指南）

要徹底解決 Stage 4，建議分兩部分修改 [`helper.c`](../../wine-winusb-shim/src/helper.c) 與 [`winusb_shim.c`](../../wine-winusb-shim/src/winusb_shim.c)：

### 修改 1：Linux 端 `helper.c` — 解開 `usb_lock` 並支援多執行緒並行傳輸

1. **將 `usb_lock` 縮小為僅保護 `handles[]` 查表**：
   寫一個輔助函式 `get_handle(uint32_t id)`，在鎖內取出 `libusb_device_handle *h` 後立即解鎖，讓 `do_control()` 與 `do_bulk()` 可以同時在不同 TCP 連線上並行執行：

```c
static libusb_device_handle *get_usb_handle(uint32_t id) {
    libusb_device_handle *h = NULL;
    pthread_mutex_lock(&usb_lock);
    if (id >= 1 && id < MAX_HANDLES)
        h = handles[id];
    pthread_mutex_unlock(&usb_lock);
    return h;
}
```

2. **`do_control()` 與 `do_bulk()` 不再持有 `usb_lock` 呼叫 `libusb_*_transfer`**：
   - 在 `do_control()` 與 `do_bulk()` 中，用 `get_usb_handle(req->handle)` 取得 `h` 後，直接呼叫 `libusb_control_transfer()` / `libusb_bulk_transfer()`，**不要**加 `usb_lock`。
   - 在 `do_bulk()` 進入 `libusb_bulk_transfer()` **之前**也先印一行 log（例如 `helper: bulk start ep=%02x len=%u timeout=%u`），這樣就能在 `/tmp/dswave-helper.log` 即時看到 Bulk 請求何時進來、期間有哪些 Control Transfer 同時發生。

3. **實作 `REQ_RESET_PIPE` / `REQ_ABORT_PIPE`（或在 `do_open` 時先 `libusb_clear_halt`）**：
   - 在 `protocol.h` 新增 `REQ_RESET_PIPE = 7`（呼叫 `libusb_clear_halt(h, (uint8_t)req->a)`）。
   - 同時建議在 `read_descriptors()` 中改用 `libusb_get_device_descriptor(dev, &desc)` + `libusb_get_active_config_descriptor(dev, &cfg)`（直接讀 Linux Kernel 快取，不在 `REQ_LIST` 時 `libusb_open` 打擾已開啟的裝置）。

---

### 修改 2：Wine 端 `winusb_shim.c` — 實作真正的 `OVERLAPPED` 非同步 Bulk 傳輸與獨立 Socket

當 `WinUsb_ReadPipe` / `WinUsb_WritePipe` 的 `overlapped != NULL` 時，**必須立即回傳 `FALSE` + `ERROR_IO_PENDING`**，並交由背景 Worker Thread 透過獨立的 TCP Socket 向 `helper` 發送 `REQ_BULK`：

#### 1. 為非同步 Bulk 傳輸建立每執行緒 / 每請求獨立的 TCP 連線輔助函式
保留原本的 `sock` 給同步的控制請求（`WinUsb_ControlTransfer` / `WinUsb_Initialize`），這樣控制傳輸永遠不會被 Bulk IN 阻塞；或者寫一個 `open_helper_socket()` 讓背景 Bulk Worker 使用自己的 Socket：

```c
static int open_helper_socket(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(HELPER_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s);
        return -1;
    }
    return s;
}
```

#### 2. 非同步 `OVERLAPPED` 狀態設定與 Worker Thread

在 Windows / Wine 的 `libusb` (`windows_winusb.c` + `windows_nt_common.c`) 中，`libusb` 會：
- 檢查 `WinUsb_ReadPipe` 回傳 `FALSE` 且 `GetLastError() == ERROR_IO_PENDING`；
- 在事件迴圈中用 `WaitForMultipleObjects` 等待 `overlapped->hEvent` 被觸發；
- 喚醒後呼叫 `WinUsb_GetOverlappedResult()` **或直接讀取 `overlapped->Internal` (NTSTATUS) 與 `overlapped->InternalHigh` (實際傳輸 byte 數)**！

因此，`bulk_transfer()` 在 `overlapped != NULL` 時應改為：

```c
#define STATUS_PENDING       ((ULONG_PTR)0x00000103L)
#define STATUS_SUCCESS       ((ULONG_PTR)0x00000000L)
#define STATUS_UNSUCCESSFUL  ((ULONG_PTR)0xC0000001L)
#define STATUS_IO_TIMEOUT    ((ULONG_PTR)0xC00000B5L)

struct async_bulk_ctx {
    uint32_t remote_id;
    UCHAR pipe_id;
    PUCHAR buffer;
    ULONG len;
    uint32_t timeout;
    int is_in;
    LPOVERLAPPED overlapped;
};

static DWORD WINAPI async_bulk_worker(LPVOID param) {
    struct async_bulk_ctx *ctx = (struct async_bulk_ctx *)param;
    int s = open_helper_socket();
    if (s < 0) {
        ctx->overlapped->InternalHigh = 0;
        ctx->overlapped->Internal = STATUS_UNSUCCESSFUL;
        MemoryBarrier();
        if (ctx->overlapped->hEvent) SetEvent(ctx->overlapped->hEvent);
        HeapFree(GetProcessHeap(), 0, ctx);
        return 0;
    }

    struct dsw_req req = {0};
    struct dsw_resp resp = {0};
    req.op = REQ_BULK;
    req.handle = ctx->remote_id;
    req.a = ctx->pipe_id;
    req.b = (uint16_t)(ctx->timeout > 0xFFFF ? 0xFFFF : ctx->timeout);
    req.c = (uint16_t)(ctx->timeout >> 16);
    req.len = ctx->len;

    int ok = 0;
    if (send_all(s, &req, sizeof(req)) == 0) {
        if (!ctx->is_in && ctx->len > 0)
            send_all(s, ctx->buffer, ctx->len);
        if (recv_all(s, &resp, sizeof(resp)) == 0) {
            if (ctx->is_in && resp.len > 0) {
                ULONG copy = resp.len <= ctx->len ? resp.len : ctx->len;
                recv_all(s, ctx->buffer, copy);
                /* 若 resp.len > copy 則讀掉剩餘 bytes */
            }
            ok = 1;
        }
    }
    close(s);

    if (ok && resp.status == 0) {
        ctx->overlapped->InternalHigh = resp.len;
        ctx->overlapped->Internal = STATUS_SUCCESS;
    } else if (ok && resp.status == -7) { /* LIBUSB_ERROR_TIMEOUT */
        ctx->overlapped->InternalHigh = resp.len;
        ctx->overlapped->Internal = STATUS_IO_TIMEOUT;
    } else {
        ctx->overlapped->InternalHigh = 0;
        ctx->overlapped->Internal = STATUS_UNSUCCESSFUL;
    }

    log_msg("Async Bulk pipe=%02x len=%u -> status=%d actual=%u\n",
            ctx->pipe_id, ctx->len, ok ? resp.status : -99, ok ? resp.len : 0);

    MemoryBarrier();
    if (ctx->overlapped->hEvent)
        SetEvent(ctx->overlapped->hEvent);

    HeapFree(GetProcessHeap(), 0, ctx);
    return 0;
}
```

而在 `bulk_transfer()` 主體中，當 `overlapped != NULL` 時：

```c
if (overlapped) {
    struct async_bulk_ctx *ctx = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*ctx));
    ctx->remote_id = s->remote_id;
    ctx->pipe_id = pipe_id;
    ctx->buffer = buffer;
    ctx->len = len;
    ctx->timeout = timeout;
    ctx->is_in = is_in;
    ctx->overlapped = overlapped;

    overlapped->Internal = STATUS_PENDING;
    overlapped->InternalHigh = 0;
    if (overlapped->hEvent)
        ResetEvent(overlapped->hEvent);

    HANDLE th = CreateThread(NULL, 0, async_bulk_worker, ctx, 0, NULL);
    if (th) {
        CloseHandle(th);
        SetLastError(ERROR_IO_PENDING);
        return FALSE;
    }
    HeapFree(GetProcessHeap(), 0, ctx);
}
```

#### 3. 更新 `WinUsb_GetOverlappedResult` 以等待 `STATUS_PENDING` 完成

在 [`winusb_shim.c`](../../wine-winusb-shim/src/winusb_shim.c#L574-L588) 中，將 `WinUsb_GetOverlappedResult` 改為支援真正的非同步狀態查詢與等待：

```c
BOOL WINAPI WinUsb_GetOverlappedResult(WINUSB_INTERFACE_HANDLE h, LPOVERLAPPED overlapped,
                                       LPDWORD transferred, BOOL wait) {
    (void)h;
    if (!overlapped) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (overlapped->Internal == STATUS_PENDING) {
        if (!wait) {
            SetLastError(ERROR_IO_INCOMPLETE);
            return FALSE;
        }
        if (overlapped->hEvent)
            WaitForSingleObject(overlapped->hEvent, INFINITE);
    }
    if (transferred)
        *transferred = (DWORD)overlapped->InternalHigh;
    if (overlapped->Internal != STATUS_SUCCESS) {
        SetLastError(overlapped->Internal == STATUS_IO_TIMEOUT ? ERROR_SEM_TIMEOUT : ERROR_GEN_FAILURE);
        return FALSE;
    }
    return TRUE;
}
```

#### 4. `WinUsb_ControlTransfer` 的 `overlapped` 處理建議
注意 `libusb` 的 `windows_winusb.c` 在 `winusbx_submit_control_transfer()` 中同樣會傳入 `overlapped`！
- 目前 `WinUsb_ControlTransfer` 因為每次只傳 2～64 bytes、耗時不到 1ms，同步完成後設定 `overlapped->Internal = 0; overlapped->InternalHigh = actual; SetEvent(overlapped->hEvent);` 並回傳 `TRUE`，`libusb` 的 `winusbx_submit_control_transfer()` 對同步回傳 `TRUE` 的情況是可以正常處理的。
- 但**前提是 `WinUsb_ControlTransfer` 使用的 `lock` 與 `sock` 絕對不能被 `WinUsb_ReadPipe` 佔用**！只要 `WinUsb_ReadPipe` 改為上述的獨立 Socket + 背景 Worker Thread，`WinUsb_ControlTransfer` 就不會再被 Bulk Read 阻塞。

---

## 四、驗證清單（下一步測試觀察重點）

完成上述兩項修改後，重新編譯 `helper` 與 `winusb.dll` 並啟動 `DSWave.exe`：

1. 檢查 [`winusb.log`]($WINEPREFIX/drive_c/ProgramData/DSWaveShim/winusb.log)：
   - 當出現 `Bulk pipe=86 len=4096` 時，應該會看到 `WinUsb_ReadPipe` **立即返回 `ERROR_IO_PENDING`**；
   - 緊接著應該會看到 DSWave 連續提交多個 `Bulk pipe=86`，或是緊接著發送後續的 `ControlTransfer`（例如 `req=0xb0` / `req=0xb1` / `req=0xb2` 輪詢觸發狀態），而不再卡死在第一筆 `Bulk pipe=86`！
2. 檢查 [`/tmp/dswave-helper.log`](file:///tmp/dswave-helper.log)：
   - 確認 `bulk ep=86` 掛在背景等待時，`req=4`（Control Transfer）仍能毫無延遲地穿插執行並回傳 `status=0`。
   - 一旦硬體觸發完成並送出 4096 bytes 波形資料，`Async Bulk pipe=86` 就會回傳 `status=0 actual=4096`，波形即可繪製到畫面上！
