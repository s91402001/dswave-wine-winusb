# USBPcap / usbmon 對照錄製

[回專案首頁](../README.md) ｜ [English root README](../README.md)

目的：取得 **Windows 上正常運作** 的 USB 行為，作為 Wine 墊片的比對基準。

> 錄下來的 `.pcap` **不進 repo**（`.gitignore` 已排除）。版本與大小記在
> 首頁 README 的「Tested versions」：`fpga1.pcap`（8,920,535 bytes）、
> `output.pcap`（129,023,508 bytes）。


要回答的三個問題：

1. App 在按「執行」前後送了哪些 `b0` / `b1` / `b2` 控制命令（完整 bytes 與順序）
2. 每筆 bulk IN 的**請求長度**與**實際完成長度**
3. 第一個資料封包是 `5,005,312`，還是別的長度（例如 2,490,368）

有了這份基準，就能判斷目前是：
- **App 的讀取大小需要調整**，還是
- **控制命令沒有正確套用到硬體**

---

## 建議路線

| 路線 | 需求 | 優點 | 限制 |
|---|---|---|---|
| **A. Windows 11 VM + USBPcap**（建議） | 你的 win11 VM、USB 直通 | 最貼近實際使用情境 | 需在 guest 安裝 Wireshark/USBPcap |
| B. Linux usbmon（Wine 端） | root | 不需 VM，可看「裝置實際在匯流排上送了多少」 | 先前 `cat` 回 `Operation not permitted`；需確認權限或改用 tshark |

兩者可同時做：A 提供「正常長什麼樣」，B 提供「Wine 這邊匯流排上實際發生什麼」。

---

## A. Windows 11 VM + USBPcap

### A1. 把裝置直通進 VM

關機後在 virt-manager 加入 USB Host Device（`2a0e:0080`），或使用 PCIe USB 控制器直通。
建議用 `usb-host`（qemu-xhci），不要用 SPICE USB redirection，避免额外轉送層影響錄製。

### A2. 在 guest 安裝

以系統管理員開啟 PowerShell：

```powershell
winget install --id WiresharkFoundation.Wireshark -e --accept-source-agreements --accept-package-agreements
```

安裝時勾選 **Install USBPcap**。
驗證：

```powershell
& "C:\Program Files\Wireshark\dumpcap.exe" -D
# 應該看到 USBPcap1, USBPcap2 ...
```

### A2.5 如果 Wireshark 沒出現 USBPcap

先跑診斷腳本（系統管理員）：

```powershell
.\04-check.ps1
```

常見原因與處理：

| 現象 | 原因 | 處理 |
|---|---|---|
| `C:\Windows\System32\drivers\USBPcap.sys` 不存在 | Wireshark 安裝時沒有勾 USBPcap（winget 預設不會裝） | 單獨安裝 **USBPcap**：<https://github.com/desowin/usbpcap/releases> → 執行 `USBPcapSetup-*.exe` → **重新開機** |
| 驅動存在但 `dumpcap -D` 沒有 USBPcapN | 首次安裝後未重開機 / 非系統管理員 | 重開機後以系統管理員再試 |
| 只有 USB 3.0 (xHCI) 控制器 | 舊版 USBPcap 對 xHCI 支援有限 | 在 VM 改接 **USB 2.0 控制器**（virt-manager → Add Hardware → USB Controller），或改用 Linux usbmon |
| 裝置是用 SPICE USB redirection | 轉送層可能讓 USBPcap 看不到 | 改用 `usb-host` 直通（見 A1） |

也可不安裝 Wireshark GUI，直接用 USBPcap 的命令列工具：

```powershell
& "C:\Program Files\USBPcap\USBPcapCMD.exe" -d \\.\USBPcap1 -o C:\usbpcap\capture.pcap
```

（`-d` 後面的介面名稱請用 `USBPcapCMD.exe --help` 或 `dumpcap -D` 確認。）

### A3. 錄製

在 guest 的 PowerShell（系統管理員）執行：

```powershell
.\01-install.ps1        # 只要第一次
.\02-capture.ps1 -Seconds 15
```

腳本會：
1. 啟動 `dumpcap` 錄製 `USBPcap1`
2. 啟動 `C:\Program Files\DSWave\DSWave.exe`
3. 等你按 Enter（或倒數結束）後停止錄製
4. 產出 `C:\usbpcap\capture-<時間>.pcapng`

按「執行」後等 5–10 秒，再停止錄製。

### A4. 抽出摘要

```powershell
.\03-extract.ps1 -Pcap C:\usbpcap\capture-xxxx.pcapng
```

會產生：
- `control.csv`：所有控制傳輸（direction、request、value、index、length、data）
- `bulk.csv`：所有 bulk 傳輸（endpoint、direction、status、length）
- `summary.txt`：筆數與長度統計

把這三個檔（或整個 pcapng）帶回 Linux 專案目錄的 `usbpcap/data/` 下。

### A5. 帶回 Linux

- 共享資料夾（virtiofs / 9p）掛載後複製，或
- `scp` / `rsync`，或
- 直接把檔案放進共用目錄

---

## B. Linux usbmon（Wine 端）

```bash
sudo usbmon/linux/record-usbmon.sh 8 /tmp/wine.mon
# 另開一個終端機啟動 DSWave 並按執行
# 停止：Ctrl-C
```

若出現 `Operation not permitted`，先確認：

```bash
mount | grep debugfs
ls -l /sys/kernel/debug/usb/usbmon/
sudo dmesg | tail
```

替代方案（需要 tshark）：

```bash
sudo tshark -i usbmon8 -w /tmp/wine.pcapng
```

解析文字檔：

```bash
python3 usbpcap/linux/parse-usbmon.py /tmp/wine.mon > /tmp/wine-summary.txt
```

---

## 分析重點

### 控制命令
- `b0` OUT：3 bytes 的暫存器寫入（位址 + 值）
- `b1` / `b2`：狀態/校正讀取
- 特別注意「按執行」前後出現的序列，以及 `b2` 64-byte 讀回的內容

### Bulk
- 請求長度 vs 完成長度
- 是否在結尾出現 ZLP（完成長度 < 請求長度且為 0）
- 是否出現負的 URB status（`-ETIMEDOUT` / `-EPIPE` / `-EIO`）

### 判讀
| 觀察 | 結論 |
|---|---|
| Windows 也只送 2,490,368 / 655,360 | 裝置行為與 App 參數不符 → 查控制命令 |
| Windows 每次都是完整的 5,005,312，且無錯誤 | Wine 端控制命令或時序不同 |
| Windows 有 ZLP、Wine 沒有（或反之） | 傳輸語意差異，需要調整 helper |
| Windows 有額外前置命令 | 我們的命令序列缺少步驟 |

---

## 檔案

```
usbpcap/
├── README.md
├── windows/
│   ├── 01-install.ps1
│   ├── 02-capture.ps1
│   ├── 03-extract.ps1
│   └── 04-check.ps1      # USBPcap 安裝診斷
├── linux/
│   ├── record-usbmon.sh
│   └── parse-usbmon.py
├── analyze/
│   └── compare.py
└── data/            # 放回來的 pcapng / csv
```

`analyze/compare.py` 可比較兩份擷取的控制序列與 bulk 長度：

```bash
python3 usbpcap/analyze/compare.py usbpcap/data/windows.csv usbpcap/data/wine.csv
```
