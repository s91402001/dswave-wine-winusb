# FPGA 上傳序列：Windows 成功流程 vs `dswfpga` 重放

輸入：`usbpcap/data/fpga1.pcap`（解壓後 8,920,535 bytes）。
對照：`tools/dswfpga.c`（2026-10-30 版）。
方法：`parse-usbpcap.py --csv` + 時間窗篩選（上傳點 ±1.5 s），control IN 的 data stage 另行直接解析 pcap。

時間原點 `t0` = 466,408-byte bulk OUT 的時刻（1793325226.715910）。

---

## 1. Windows 成功序列（逐筆，含 b2 回值）

| Δt (s) | 傳輸 | 內容 | b2 IN 回值 |
|---|---|---|---|
| −0.0887 | CTL out `40 b1` data=`f00200` | programmer start | — |
| −0.0635 | CTL out `c0 b2` len=2 → IN | **status read** | `10 31` |
| −0.0627 | CTL out `40 b1` data=`f20200` | programmer stop? | — |
| −0.0477 | `c0 b2` len=2 | status read | `00 31` |
| −0.0470 | CTL out `40 b1` data=`ff0200` | | — |
| −0.0325 | `c0 b2` len=2 | status read | **`03 00`**（未設定） |
| −0.0320 | CTL out `40 b0` data=`fefeff` | | — |
| −0.0173 | CTL out `40 b0` data=`fe0100` | | — |
| −0.0127 | CTL out `40 b1` data=`ff0200` | | — |
| −0.0012 | `c0 b2` len=2 | status read | `03 00` |
| −0.0007 | CTL out `40 b0` data=`fce81d` | FPGA PROG（脈衝寬 0x1DE8） | — |
| −0.0004 | CTL out `40 b0` data=`fd0700` | FPGA init | — |
| **±0** | **BULK out 0x02 len=466408** | bitstream，status 0 | — |
| **+0.0638** | **BULK in 0x02(len→0x82) len=0** | **zero-length IN！** | — |
| +0.0640 | CTL out `40 b1` data=`ff0200` | | — |
| +0.0767 | `c0 b2` len=2 | status read | **`83 00`** ← 出現 0x80 位（已設定） |
| +0.1839 | CTL out `40 b1` data=`f10200` | | — |
| +0.1995 | `c0 b2` len=2 | status read | `00 31` |
| +0.1997 onward | `b0 fb…`/`b1 fb…` + `c0 b2` len=16 | 一般初始化 / SCPI（b2 開始回 16 bytes 有效資料） | — |

要點：

* `fd 07 00` 與 bulk OUT 之間**沒有刻意延遲**（0.3 ms），上传後到 ZLP 之間 64 ms。
* 判定「已設定」的訊號是 `b2` 兩 bytes 讀取的第一位元組 `0x03 → 0x83`（0x80 位）。
* 每個 b1/b0 指令之間 Windows 都穿插一次 `b2` 讀取（間隔 ~15 ms）。
* 全 pcap 中 `b1 f0 02 00` 只出現 **1 次**（序列最開頭）；上傳之後 Windows **沒有**再送 f0。

---

## 2. 與 `dswfpga.c` 的逐行差異

> ⚠️ 本節對照的是 **v1 版** dswfpga（歷史記錄）；v3 已按 §3 修正節實作，請以 §3 為準。

| # | Windows | dswfpga（現在） | 差異 |
|---|---|---|---|
| 1 | `b1 f0 02 00` | ✅ 相同 | — |
| 2 | `b2` 讀（2B）→ 记录回值 | ❌ 跳過 | **缺 status read** |
| 3 | `b1 f2 02 00` | ✅ | — |
| 4 | `b2` 讀 | ❌ | 缺 |
| 5 | `b1 ff 02 00` | ✅ | — |
| 6 | `b2` 讀（`03 00`） | ❌ | 缺 |
| 7 | `b0 fe fe ff` / `b0 fe 01 00` / `b1 ff 02 00` | ✅ | — |
| 8 | `b2` 讀 | ❌ | 缺 |
| 9 | `b0 fc e8 1d` → `b0 fd 07 00` → 立即 OUT | ✅ | 延遲不是因素 |
| 10 | 上傳後 +64 ms：**EP2 zero-length IN** | ❌ 完全沒有 | **候選主因** |
| 11 | `b1 ff 02 00` → `b2`（`83 00`）→ `b1 f1 02 00` → `b2` | 有 ff、f1，但無 b2 poll | 缺 poll；無法判定成功 |
| 12 | （無） | **多送 `b1 f0 02 00`** | **多出的指令**，f0 是 programmer start，結尾重送可能把載入器狀態重開 |

---

## 3. 假設排序（Task 6 依序驗證）

> **2026-10-30 Task 6 結果（修正本節）**：假設 1 已證偽 — 裝置
> **沒有 EP 0x82**（描述符只有 `0x02 OUT / 0x86 IN / 0x84 IN`），且
> 0-length bulk 傳輸根本不會上 bus，pcap 裡那筆「ZLP IN」是 host 端
> 完成的記錄。真正關鍵是**假設 3 的反面**：Windows 在 `fc e8 1d`(PROG)
> 之前一定先讀到 `b2 = 03 00`（idle）；我們的 v1 在 `01 00` 狀態下
> 直接 PROG+上傳 → 永不設定。v3 = 指令間 15 ms + PROG 前輪詢等待
> `03 00` + 整段序列最多 3 次重試 → **首次嘗試即 `83 00` CONFIGURED**
> （不需 USB reset、不需 ZLP、不需 VM）。

1. **缺 EP2 zero-length IN**：FPGA 載入協定可能需要一個 ZLP IN 作為「資料結束/沖刷」；沒有它，bitstream 進得去但載入器不收尾 → 上傳 status=0 但 b2 仍 `03 00`。與我們的觀察完全吻合。
2. **結尾多送的 `b1 f0 02 00`**：把 f0（programmer start）當結尾指令是誤讀；可能在 fd 07 00/ff 之後又把 programmer 重啟。
3. **缺 b2 poll**：不影響設定與否，但沒有它我們無法判定；Windows 靠 b2（0x80 位）判斷完成。
4. ~15 ms 的指令間隔：可能性低（Windows 從 fc/fd 到 OUT 根本沒有延遲）。

---

## 4. dswfpga v2 規格（Task 6 實作）

> ⚠️ **已被 §3 修正節取代**：v2 的 ZLP 與「dswbulk 直接擷取」兩項不成立；最終規格見 `tools/README.md` 的 dswfpga 段落。

1. 保留現有 8 條前置指令（順序已與 Windows 一致），每條之後做 `b2` 2-byte 讀並印出回值。
2. 移除結尾的 `b1 f0 02 00`。
3. 上傳成功後：等 ~100 ms → `libusb_bulk_transfer(h, 0x82, buf, 0, ...)`（ZLP IN）→ `b1 ff 02 00` → 輪詢 `b2`（每秒、上限 10 s）直到 byte0 & 0x80，印出每次回值。
4. 驗收：b2 出現 `83 …`（或 0x80 位）= 成功；後續 `dswbulk 5 20` 可直接擷取。

## 5. b2 回值速查

| 回值 | 情境 |
|---|---|
| `03 00` | FPGA 未設定 |
| `83 00` | FPGA 已設定（0x80 = configured） |
| `10 31` / `00 31` | programmer 活動期間 |
| len=16、有資料 | 正常執行中（b2 同時是 SCPI/status 通道） |
