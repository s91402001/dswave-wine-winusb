# dswave-wine-winusb

在 Linux 上以 **Wine + 使用者模式 WinUSB 墊片**，執行 DreamSourceLab
**DSCope M454**（`2a0e:0080`）的 Windows 版 **DSWave**。

[English](README.md) | **繁體中文**

> **狀態（2026-10-30）** — 裝置列舉、擷取與 Start/Stop 均可用：
> 連續 30 秒擷取與多次 Run→Stop→Run 已驗證。FPGA bitstream 在 Wine 下
> 仍無法完成設定（每次斷電後需要用 Windows VM 設定一次）。

---

## 可用與不可用

| 項目 | 狀態 | 說明 |
|---|---|---|
| 裝置列舉 | ✅ | 合成 PnP 樹 + hub IOCTL |
| `H0: DSCope M454`、READY | ✅ | FPGA 已設定時顯示 `Fpga is already done` |
| 擷取 / 波形 | ✅ | 30 秒以上、修正後 0 次 `-EPROTO` |
| `Stop` → 再 `Run` | ✅ | 已驗證（2026-10-30，三次 Run/Stop 循環，CancelIoEx hook 乾淨取消） |
| FPGA bitstream 上傳 | ❌ | 傳輸完成但 FPGA 未實際設定（見 Roadmap） |
| 免 Windows 重新插拔 | ❌ | 需要 Windows VM 設定 FPGA 一次 |

---

## 為什麼需要

* Wine 的 `winusb.dll` 全是 stub，`wineusb.sys` 只是 bus driver：
  沒有 WinUSB function driver、沒有使用者模式可開啟的裝置介面，
  Windows 應用程式完全看不到 WinUSB 裝置。
* DSWave 以**絕對路徑**載入 `C:\windows\system32\{SetupAPI,WinUSB}.dll`，
  再用 `GetProcAddress` 取 API，所以把 DLL 放在程式目錄沒有用，
  必須覆蓋 `system32`。

---

## 架構

```
DSWave.exe（靜態連結 libusb-win32 / libusb）
  |
  +-- SetupDi* / CM_*  -->  setupapi.dll + cfgmgr32.dll 墊片
  |                         合成 PnP 樹：
  |                           HCD 0x3000 -> HUB 0x2000 -> DEV 0x1000
  |                         裝置介面路徑 = 真實空檔案
  |                         （CreateFile 可開啟，不經過核心驅動）
  |
  +-- WinUsb_*          -->  winusb.dll 墊片（20 個進入點 + 輔助函式）
                             |
                             |  TCP 127.0.0.1:47123（長度前綴 RPC）
                             v
                        helper（原生 Linux，libusb-1.0）
                             |
                             v
                        /dev/bus/usb/BBB/DDD（真正的 DSCope M454）
```

Windows 端另外掛了三個 IAT hook：

| 匯入函式 | 用途 |
|---|---|
| `DeviceIoControl` | 回答 hub IOCTL（`GET_NODE_CONNECTION_INFORMATION_EX`、`GET_DESCRIPTOR_FROM_NODE_CONNECTION` 等） |
| `CreateIoCompletionPort` | 記住 IOCP，讓傳輸可以送出完成通知 |
| `CancelIoEx` | 直接把 pending 傳輸標記為取消並完成（libusb 的取消路徑在純檔案上會靜默失敗） |

RPC 協定、bulk 傳輸生命週期與狀態碼對照請見
[`docs/architecture.md`](docs/architecture.md)。

---

## 需求

* Linux（x86_64）、Wine（以 `devel` 分支測試，Wine 11.x）
* `libusb-1.0` 開發檔
* `mingw-w64` 交叉編譯器（`x86_64-w64-mingw32-gcc`）
* Python 3（`scripts/gen_def.py` 使用）
* 已安裝 **DSWave v1.1.0** 的 Wine prefix
* 已設定好 FPGA 的裝置（見狀態表）

---

## 建置

```bash
cd wine-winusb-shim
./scripts/build.sh
```

`WINE_LIB` 預設為 `/opt/wine-devel/lib/wine/x86_64-windows`，可覆寫：

```bash
WINE_LIB=/usr/lib/wine/x86_64-windows ./scripts/build.sh
```

輸出（`wine-winusb-shim/build/`）：

| 檔案 | 用途 |
|---|---|
| `setupapi.dll` | 列舉 + hub IOCTL 墊片（其餘 597 個匯出 forward 回原版） |
| `cfgmgr32.dll` | `CM_Get_Parent` / `CM_Get_Child` 墊片 |
| `winusb.dll` | WinUSB API 墊片（必須用 `-Wl,--export-all-symbols` 連結） |
| `helper` | 原生 Linux libusb RPC daemon |

## 安裝

```bash
cd wine-winusb-shim
WINEPREFIX=~/.wine-dswave ./scripts/install.sh
```

腳本會把 DLL 複製到 `system32`、設定 `native` DLL override、備份 Wine
原始檔，並在背景啟動 helper。

## 執行

```bash
# helper（install.sh 會自動啟動；重新編譯後需手動重啟）
cd wine-winusb-shim
DSW_DEBUG=1 DSW_MBPS=20 setsid nohup ./build/helper >/tmp/dswave-helper.log 2>&1 &

# 應用程式
WINEPREFIX=~/.wine-dswave wine "C:\\Program Files\\DSWave\\DSWave.exe" -l 5
```

移除：`WINEPREFIX=~/.wine-dswave ./scripts/uninstall.sh`。

---

## 環境變數

| 變數 | 預設 | 說明 |
|---|---|---|
| `DSW_DEBUG` | `0` | helper 與 libusb 詳細日誌 |
| `DSW_MBPS` | `0` | 讀取速率上限（MB/s，`20` 對齊 Windows 基準） |
| `DSW_CHUNK_BYTES` | `405504` | bulk IN 傳輸大小 |
| `DSW_FILL` | `1` | 串接裝置封包直到填滿呼叫端緩衝區 |
| `DSW_NO_RESET` | `0` | 第一次開啟時不做 `libusb_reset_device()` |
| `DSW_LIST_LIVE` | `0` | 關閉 descriptor 快取（僅除錯） |
| `DSW_SOFT_RESET` | `0` | 關閉裝置時重放 stop 序列 |
| `DSW_FPGA_RECOVER` | `0` | bitstream 上傳前先 reset + 重放前置序列 |
| `DSW_CANCELIO_HOOK` | `1` | 啟用 `CancelIoEx` hook |

---

## 疑難排解

| 症狀 | 原因 / 處理 |
|---|---|
| 開啟時 `LIBUSB_ERROR_ACCESS` | `winusb.dll` 匯出遺失 — 用 `-Wl,--export-all-symbols` 重建，並以 `objdump -p build/winusb.dll \| grep WinUsb_` 確認 |
| `claim interface` 回 `LIBUSB_ERROR_BUSY` | `virt-manager` / QEMU 仍持有裝置，解除 USB redirection |
| READY 但 `Run` 沒資料 | 裝置卡死：用 Windows VM 重新設定 FPGA，或重新插拔 |
| `Stop` 時 UI 凍結 | 設 `DSW_DEBUG=1`，檢查 `cancelio.log` 與 helper log |
| 波形約 1 秒後停止 | 在 helper log 找 `-EPROTO`（urb status `-71`） |

日誌位置：

* `/tmp/dswave-helper.log`
* `${WINEPREFIX}/drive_c/ProgramData/DSWaveShim/winusb.log`
* `${WINEPREFIX}/drive_c/ProgramData/DSWaveShim/setupapi.log`
* `${WINEPREFIX}/drive_c/ProgramData/DSWaveShim/cancelio.log`

---

## 測試版本

下列二進位檔**不在 repo 內**，只記錄版本。

| 檔案 | 大小（bytes） | 用途 |
|---|---|---|
| `DSWave_v1.1.0_x64_setup.exe` | 48,637,552 | 廠商安裝檔（v1.1.0, x64） |
| `output.pcap` | 129,023,508 | Windows USBPcap 基準（一般擷取） |
| `fpga1.pcap` | 8,920,535 | Windows USBPcap 基準（FPGA 設定 + 擷取） |

兩份擷取檔以 **gzip 壓縮**形式收在
[`usbpcap/data/`](usbpcap/data/README.md)（2.0 MB 與 29.8 MB）；安裝檔不重新散布。

* 裝置：`2a0e:0080`，USB 3.0 SuperSpeed（5 Gb/s）
* Wine：`devel` 分支，11.x
* Windows 基準傳輸：bulk IN 405,504 bytes、間隔約 22 ms（≈18.6 MB/s）

錄製方式：[`usbpcap/README.md`](usbpcap/README.md)。

### 參考擷取檔

兩份都在 Windows 11（VM）以 `USBPcapCMD.exe` 錄製，並以 gzip 壓縮收在
[`usbpcap/data/`](usbpcap/data/README.md)。

| 擷取檔 | 情境 | 長度 / 封包數 | 內容 |
|---|---|---|---|
| `fpga1.pcap` | **冷裝置**（剛重插、FPGA 尚未設定）→ 開啟 DSWave → FPGA bitstream 上傳 → SCPI 設定 → 擷取一次 → 按 Stop | 13.47 秒 / 2,818 | FPGA 上傳（`0x02` 單筆 bulk OUT 466,408 bytes）、405,504 bytes 的 bulk IN（間隔約 22 ms）、完整的 start/stop 控制序列、pending read 以 `0xC0010000` 結束 |
| `output.pcap` | **一般擷取**：裝置已設定（無 FPGA 上傳）→ DSWave 啟動並連續擷取 → 停止 | 41.89 秒 / 7,881 | 持續的 bulk IN 流量，用於傳輸大小/時序統計；4096 bytes 標頭（`type=1`）、無傳輸錯誤 |

`fpga1.pcap` 使用的設定：TimeBase 2 ms、400 kpts、類比 5 MSa/s、數位 100 MSa/s
—— 也是 [`docs/progress.md`](docs/progress.md) 所有比對所用的設定。


---

## 開發筆記

* **bulk 串流期間絕對不要碰 EP0。** App 的熱插拔執行緒每 ~100 ms 列舉一次，
  每次都會開啟裝置並送 `GET_DESCRIPTOR`，會同時凍結 EP0 與 `0x86`。
  現在 descriptor 以 `bus:addr` 快取。
* **不要用短 timeout 切 bulk URB。** 反覆取消再重送會產生 `-EPROTO`
  （`urb status -71`）。現在改用非同步傳輸，timeout 直接用呼叫端給的值。
* **`OVERLAPPED.Internal` 必須是 NTSTATUS**（`STATUS_CANCELLED = 0xC0000120`），
  不是 `USBD_STATUS`（`0xC0010000`）。
* **FPGA loader 要求整包 bitstream 一次傳完。** 把 466,408 bytes 切成
  405,504 + 60,904 會得到 `LIBUSB_ERROR_IO`。

---

## 專案結構

```
docs/                 進度、計畫、架構、分析文件
tools/                獨立診斷工具（dswbulk、dswfpga）
wine-winusb-shim/     墊片本體（src/ + scripts/）
usbpcap/              擷取與解析工具（Windows / Linux）
```

## Roadmap

1. ~~完成 `Stop` → 再 `Run` 的修正~~ — 已於 2026-10-30 完成，
   見 [`docs/progress.md`](docs/progress.md)。
2. 讓 FPGA bitstream 在 Wine 下真正完成設定。序列差異已分析完成：
   見 [`docs/analysis/fpga-upload-sequence.md`](docs/analysis/fpga-upload-sequence.md)
   （上傳後缺 **`0x82` zero-length IN** 與 `b2` 完成輪詢）；
   先用 `tools/dswfpga` 原生驗證，再移植進 helper。
3. 裝置重新列舉時通知 App（`WM_DEVICECHANGE`）。
4. 選配：自動化擷取 smoke test。

## 授權

MIT — 見 [LICENSE](LICENSE)。

## 免責

本專案為互通性實作：針對我們自行擁有/使用的應用程式，提供 Wine 端墊片。
不含任何廠商二進位檔。DSWave、DreamSourceLab、DSCope 為其各自所有者的商標。
