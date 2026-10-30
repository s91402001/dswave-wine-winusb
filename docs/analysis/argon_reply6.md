# `progress.md` 最新進度審查與改進建議 (`argon_reply6.md`)

## 一、進度判讀

16:25 的量測成果很有價值，並且**推翻了 `argon_reply5.md` 的候選 A/B**（順序、IOCP 長度都沒問題）。現在的關鍵事實是：

- 標頭 4096 bytes 只有魔數 `55555555 / 00000002 / 80000000`，無長度欄位 → `m_pkt_bytes` 由 App 自行推算。
- 同一個 5,005,312 的請求，**這次只收到 2,949,120 bytes 就結束**；先前同樣的請求卻曾完整收到 5,005,312，再下一筆只有 327,680。
  → **同一組參數下，完成長度不穩定**。若只是「控制命令套錯」，長度應該是固定的；不穩定更像是**傳輸層（helper）造成的資料遺失或截斷**。

> 因此我不建議把下一步直接押在「Windows 11 VM + USBPcap」，那是最昂貴的選項；有更便宜且更能分辨問題的做法（見第三節）。

---

## 二、對目前判斷的檢討

### 1. 「短封包不再立即結束」的修改建議**還原**

[`progress.md`](../progress.md) 提到 `helper.c` 改成短封包 / ZLP 時繼續累積到請求長度或 deadline。這**偏離 WinUSB 與 libusb 的語意**：
- 真實 WinUSB 與 `libusb_bulk_transfer` 都在短封包 / ZLP 時完成該筆 transfer；App 的流程本來就預期「一筆完成 = 一個邏輯單位」。
- 繼續累積會把**下一個影格的標頭與資料併入同一筆**，反而製造 packet 邊界錯位（正是 assert 想檢查的東西）。
- 目前只因為 `xfer == 0` 的退出條件才沒惡化。請還原為「短封包即完成」。

> 注意：2,949,120 = 4096×720，恰為 512 / 1024 的整數倍，因此它是靠 **ZLP** 結束而非一般短封包；兩者都應該立即完成。

### 2. 最可疑的新根因：helper 的「200 ms 切片 + 逾時取消」本身會丟資料或截斷

`libusb_bulk_transfer(..., slice=200ms)` 逾時時，libusb 會**取消 URB 再重送**。對 5 MB、串流式的 Bulk IN：
- 取消的瞬間，xHCI 可能已把部分 packet 收進控制器 FIFO；取消後這段資料可能被丟棄，下一個 URB 從別處接上 → **串流出現缺口**。
- 同一請求的完成長度因此取決於「剛好第幾個 200 ms 切片逾時」→ 完全吻合「長度每次不同（5,005,312 / 2,949,120 / 327,680）」。
- 又，等待觸發階段（「等待信號…」）裝置本來就可能長時間無資料，切片逾時**必然發生很多次**。

**建議（高優先）**：helper 的 Bulk IN 改用 **libusb 非同步 transfer**，不使用逾時切片：
```c
struct libusb_transfer *t = libusb_alloc_transfer(0);
libusb_fill_bulk_transfer(t, h, ep, buf, len, cb, &done, 0 /* 無逾時 */);
libusb_submit_transfer(t);
/* 專用 event thread 跑 libusb_handle_events();
   等待 cb 完成；abort 時才 libusb_cancel_transfer(t) */
```
- 只有 `REQ_ABORT_PIPE` / 關閉 handle 才取消，一般擷取**零取消、零重送**。
- 這也更貼近 WinUSB 真實行為（`PIPE_TRANSFER_TIMEOUT = 0` 即無限等待）。
- 逾時改由 App（libusb 的 `WinUsb_AbortPipe`）主導，不需要 helper 自己切片。

**快速驗證（半小時）**：先在 helper 的 bulk 日誌加上「本筆發生幾次切片逾時 / 每次 xfer」，若看到 2,949,120 那筆曾經歷逾時切片 → 證實；再改非同步比較。

### 3. `seq=2 status=-1 (LIBUSB_ERROR_IO)`
這不一定是裝置問題，可能是前一筆截斷後 App 中止、或 endpoint 被 halt。請同時檢查 `dmesg -w`（xhci / usb 錯誤）並在 IO 錯誤時記錄是否剛 `clear_halt`。

---

## 三、更便宜的「對照基準」：用 Linux 原生 DSView + usbmon，不必先開 Windows VM

目的是取得「正常行為」的 b0/b1/b2 控制序列與 bulk 長度。DSCope 在 Linux 有原生 DSView 支援：

1. 安裝／執行原生 Linux 版 DSView（或自編 DreamSourceLab 的 DSView），**用相同設定**（同樣的 100 µs/div、通道、耦合）按執行。
2. 以 usbmon 錄下匯流排：
   ```bash
   sudo modprobe usbmon
   sudo cat /sys/kernel/debug/usb/usbmon/8u > /tmp/native.mon      # 8 = bus 號
   # 或 sudo tshark -i usbmon8 -w /tmp/native.pcapng
   ```
   （目前 `/sys/kernel/debug` 無權限，需 sudo 或掛載 debugfs；`usbmon` 模組亦未載入。）
3. 再用同樣方式錄 **Wine + shim** 的一次擷取：`/tmp/wine.mon`。
4. 比對：
   - `b0/b1/b2` 控制序列 bytes 與順序（尤其「啟動」前後）
   - 每筆 bulk IN 的**實際完成長度**與是否出現 ZLP
   - 第一筆資料是否同樣是 4096 標頭 + 同樣長度的 payload

usbmon 是**匯流排層**真相：若 Wine 版的 bulk 在匯流排上就比 native 少，問題在控制命令；若匯流排上長度與 native 相同、只是 helper 回報變少 → 確定是 helper 切片/取消造成的。這一步就能一次分辨兩種假設，且不必架 VM。

Windows VM + USBPcap 保留為最後備案（需要 USB passthrough，且 VM 環境自身也可能改變行為）。

---

## 四、對 `m_pkt_bytes` 的補充

- 因為標頭無長度欄位，App 預期的 5,005,312 是由取樣參數算出。若 native/Windows 實際也只送 2,949,120，代表 **App 在取樣率/深度的計算與裝置模式有差異**（如缺少對 `0xb2` 讀取結果的正確回應）。
- 請檢查 App 讀 `0xb2` 的 IN 控制傳輸（len=2/16/64）**回來的內容**：目前 log 只記錄長度。將這些 IN 資料（hex）也記入日誌，並與 native 錄到的對比；若 shim 在 IN 傳輸時有截斷、位元組序、或把 `transferred` 當成請求長度，會導致 App 算錯。

---

## 五、建議執行順序

1. **還原**「短封包繼續累積」的修改。
2. helper bulk 日誌加：每筆的切片逾時次數、各次 `xfer`、總長度（確認假說 2）。
3. **Bulk IN 改為 libusb 非同步 transfer，取消僅限 abort/close**；重測看完成長度是否穩定為同一個值。
4. 錄 usbmon（Wine 版 vs 原生 DSView）比對控制序列與 bulk 長度。
5. 把 `0xb2` IN 控制傳輸的 payload 記入日誌並比對。
6. 以上都無法解釋時，才啟動 Windows VM + USBPcap。

> [!IMPORTANT]
> 重點：**「同一請求每次完成長度不同」是傳輸層問題的強烈訊號**。在確認 helper 不會因逾時取消而丟資料之前，不應認定是控制命令錯誤。
