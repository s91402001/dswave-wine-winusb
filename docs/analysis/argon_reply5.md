# `progress.md` 現況審查與「尚未做到部分」改進建議 (`argon_reply5.md`)

## 一、現況確認

依 [`progress.md`](../progress.md) 最新章節（16:05）：

| 項目 | 狀態 |
|---|---|
| 假 PnP 樹、Descriptor、FPGA/校正初始化 | ✅ |
| 非同步 OVERLAPPED + worker pool + abort/reset | ✅ |
| 動態位址（重列舉 8:6→8:7→8:8）、IOCP 完成通知 | ✅ |
| 5,005,312 bytes bulk IN 實際收到取樣資料 | ✅ |
| **波形渲染** | ❌ `msopackethandler.cpp:266` assert：`m_pkt_bytes <= buf_bytes - head_bytes` |

之後已不是「傳輸失敗」，而是 **資料流的邊界（封包切分 / 順序 / 完成長度）與 App 預期不一致**。

---

## 二、assert 的候選根因（依可能性排序）

### 候選 A（最可疑）：同 Endpoint 多筆 transfer 的完成順序不保證 = 提交順序

- 計畫中「per-`(handle, ep)` FIFO 鎖」是在 **worker 取得 ctx 之後**才搶 `ep_lock`。
  但 worker pool 取佇列與搶鎖的順序取決於執行緒排程，**第 2 筆可能先於第 1 筆拿到 `ep_lock`**。
- DSWave 會預先掛多筆 `0x86` 讀取（ring）。若順序被交換，
  「標頭（4096，`55 55 55 55 …`）」可能被填進第 2 筆 buffer，
  App 就會把資料當成別的封包解析 → 正好符合「`m_pkt_bytes` 超過剩餘 buffer」。
- 現象吻合：第 1 筆 4096（標頭）、第 2 筆 5,005,312、第 3 筆 327,680（短讀）——
  App 實際想要的可能是標頭 + 一整包資料，卻被切成不同的邊界。

**建議（排序保證）**
1. 在 `WinUsb_ReadPipe` **提交當下**（呼叫端執行緒、持鎖）為每個 `(handle, ep)` 配發遞增序號 `seq`：
   ```c
   ctx->seq = ep_next_seq[pipe]++;   /* 在 enqueue 前，submit 執行緒內 */
   ```
2. 序號隨 `shim_req`（例如 `d` 欄位）送到 helper；helper 對每個 `(handle, ep)` 維護 `serving_seq`，
   用 **ticket lock**（`pthread_cond_wait` 直到 `my_seq == serving_seq`）取代單純 mutex，
   完成後 `serving_seq++` 並 broadcast。
3. abort / 逾時路徑也必須推進 `serving_seq`（否則後面的 transfer 會永遠卡住）。
4. worker pool 佇列本身也要是 FIFO（確認用 queue 而非 stack / 隨機喚醒）。

**驗證**：在 `winusb.log` 與 helper log 都印 `seq`；完成序必須嚴格遞增。

### 候選 B：完成長度（`InternalHigh` / IOCP 的 bytes）與實際資料不一致

- libusb（Windows 後端）從 IOCP 取得完成封包後，使用 `overlapped->InternalHigh`
  （或 `GetQueuedCompletionStatus` 的 `lpNumberOfBytes`）作為 `actual_length`。
- 請確認 `PostQueuedCompletionStatus(iocp, bytes, key, overlapped)` 的 `bytes` **等於實際收到長度**，
  而非請求長度；且 `Internal` 寫成 `STATUS_SUCCESS` 時 `InternalHigh` 已先寫入（寫入順序：
  `InternalHigh` → `MemoryBarrier()` → `Internal` → `SetEvent` / `PostQueued…`）。
- 第 3 筆「327,680 短讀」要確認：App 收到的 `actual_length` 是否 = 327,680，而不是 5,005,312。

### 候選 C：短封包（short packet）語意與 libusb_bulk_transfer 的差異

- 真正的 WinUSB `ReadPipe`：在收到 **短封包**（< wMaxPacketSize）或達到長度時完成。
- Linux `libusb_bulk_transfer` 行為相同，但 helper 的 **200 ms 切片累加**會改變語意：
  切片逾時（`LIBUSB_ERROR_TIMEOUT` 且 `xfer>0`）後**繼續讀**，可能把下一個邏輯封包的資料併進同一筆 transfer，
  導致一筆完成跨越封包邊界（App 預期在短封包處結束）。
- 建議在 helper 區分：
  - 若切片逾時時 `xfer` 是 `wMaxPacketSize`（HS 512 / SS 1024）的**非整數倍**：視為短封包，**立即完成**。
  - 只有在 `xfer` 為整數倍且尚未達到 `len` 時才繼續累加。
- 同時檢查 `libusb_bulk_transfer` 超大長度（5 MB）在 usbfs 會被拆成多個 URB，
  逾時取消時，已完成的區塊與取消區塊的邊界要靠 `transferred` 確認，勿假設整數倍。

### 候選 D：App 讀取大小策略與裝置預期不同（需要看 DSView 原始碼協定）

- 已觀察到 4096-byte 標頭 `55 55 55 55 02 00 00 00 00 00 00 80 …`。
- 請把標頭 **至少 64 bytes** 全部 dump，與 DreamSourceLab 公開原始碼（`dsl_mso` / `msopackethandler`，
  `m_pkt_bytes`、`head_bytes`）對照：判斷「標頭宣告的 payload 長度」是否 = 5,005,312 − 某偏移。
- 若 payload 宣告長度 + 標頭 ≠ 5,005,312（例如差 4096），表示 App 的讀取長度本來就是「標頭+payload」，
  而我們把標頭與 payload 拆成兩筆完成事件（見候選 A/C）。

---

## 三、建議的診斷順序（先便宜、後昂貴）

1. **在 shim / helper 加「流水號 + 長度」日誌**（成本最低，立刻可做）：
   `submit seq=N ep len` → `start seq=N` → `done seq=N total=M head=<前 16 bytes>` → `post seq=N bytes=M`。
   - 若出現 `done` 順序亂序 ⇒ 候選 A，直接做 ticket lock。
2. **離線重現**：寫一個 Python/C 客戶端連 helper，對 `0x86` 依序送 `REQ_BULK`
   （4096、5,005,312、…），單執行緒同步執行，看資料是否 = 「標頭 + 完整 payload」。
   - 單執行緒正常、多執行緒異常 ⇒ 100% 是順序問題（候選 A）。
3. **暫時退回診斷模式**：把 worker pool 設成 1 條（序列化），重測波形。
   - 若 assert 消失，即證實為排序問題；之後再以 ticket lock 恢復並行。
4. 解標頭欄位（候選 D），核對 `m_pkt_bytes`。

---

## 四、其他「目前還不行 / 容易出問題」的部分與建議

| 項目 | 風險 | 建議 |
|---|---|---|
| **assert 之後執行緒卡死** | 一旦 App 異常就無法收尾 | 保證 `AbortPipe`/`CloseHandle` 能在 1 秒內喚醒所有 in-flight worker；加入 shim 層看門狗（超過 N 秒無完成事件就 log 並 abort） |
| **停止擷取 / 重複執行** | 重列舉位址變動、handle 洩漏 | 補驗證：連續 Run/Stop ×10、拔插後重連；helper 輸出 `handles=N` 應回到 0 |
| **日誌量** | `winusb.log` 每筆控制傳輸都寫檔（數千行），擷取期間拖慢 | 預設只記錄錯誤與 bulk；以環境變數 `DSW_SHIM_LOG=2` 開啟詳細模式 |
| **PnP 假裝置寫死 3 節點** | 多裝置 / 不同機種（DSLogic、DSCope 其他型號）失敗 | 依 helper `REQ_LIST` 回報的裝置數動態產生節點 |
| **IOCP hook 以 IAT 修補** | 若 libusb 以延遲載入或 `GetProcAddress` 取得函式則失效 | 啟動時檢查 hook 是否命中並記錄；必要時 hook `GetProcAddress` |
| **64 MiB 上限 `SHIM_MAX_XFER`** | App 若請求更大會被拒絕 | 超過上限時改為分段回傳或動態配置，並記錄明確錯誤 |
| **無自動化測試** | 每次改動靠手動點 UI | 加入 `selftest`：對 helper 做 abort / 順序 / 短封包測試，納入 `install.sh` 前置檢查 |
| **UI 自動化不穩** | 驗證耗時 | 以 App 的 `-l 5` 日誌作為判定依據（`DEVICE_RUNNING`、首包 `head`、無 assert）而非畫面截圖 |
| **文件** | `progress.md` 已 378 行，狀態分散 | 增加一份「已知限制 / 故障排除」頁，保留啟動指令與日誌路徑 |

---

## 五、下一步（建議照此順序）

1. 加流水號 / 長度 / 標頭前 16 bytes 日誌（半小時內可完成）。
2. 實驗：worker pool = 1 → 重測。若 assert 消失 → 實作 ticket lock（候選 A）。
3. 若仍 assert：dump 標頭 64+ bytes，對照 `m_pkt_bytes` 欄位；檢查短封包處理與 IOCP 回報 bytes（候選 B/C）。
4. 通過後再處理停止/重跑、日誌降噪、多裝置化。

> [!NOTE]
> 以上根因皆為依 `progress.md` 現象推論，尚未讀取最新 `winusb_shim.c` / `helper.c` 實作與 `winusb.log` 驗證；
> 建議先做第三節步驟 1 取得序號日誌，再決定是否需要全部修改。
