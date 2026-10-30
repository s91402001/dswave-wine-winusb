# DSWave WinUSB shim for Wine

讓 Windows 版 DSWave 在 Wine 下透過 Linux libusb 與 DreamSourceLab 裝置
（`2a0e:0080`，DSCope M454）通訊的使用者模式墊片。

[回專案首頁](../README.md) ｜ [English root README](../README.md) ｜
[架構細節](../docs/architecture.md)

## 為什麼需要

* Wine 的 `winusb.dll` 全部是 stub。
* Wine 的 `wineusb.sys` 只是 bus driver，沒有 function driver、沒有 user-mode
  可開啟的裝置介面 → Windows 端看不到任何 WinUSB 裝置。
* DSWave 以**絕對路徑** `C:\windows\system32\{SetupAPI,WinUSB}.dll` 動態載入
  並用 `GetProcAddress` 取得 API → 只把 DLL 放在程式目錄沒用，
  必須覆蓋 system32 + DLL override。

## 架構

```
DSWave.exe (libusbK/libusb 靜態連結)
  ├─ SetupDi*/CM_*  ── setupapi.dll + cfgmgr32.dll 墊片
  │                     合成 PnP 樹 HCD(0x3000) → HUB(0x2000) → DEV(0x1000)
  │                     裝置路徑指向真實空檔案（CreateFile 可開啟）
  └─ WinUsb_*       ── winusb.dll 墊片 ── TCP 127.0.0.1:47123
                                            └─ helper（原生 Linux, libusb）
                                                └─ /dev/bus/usb
```

* `src/setupapi_shim.c`：只攔截 USB/WinUSB 相關列舉，其餘 597 個匯出以
  `.def` forward 回 `setupapi_orig.dll`。
* `src/ioctl_hook.c`：IAT hook `DeviceIoControl`（hub IOCTL）、
  `CreateIoCompletionPort`、`CancelIoEx`（讓取消能立即完成 pending 傳輸）。
* `src/winusb_shim.c`：**20 個 `WinUsb_*` + `shim_cancel_overlapped`**。
  `winusb.dll` 必須以 `-Wl,--export-all-symbols` 連結，否則 mingw 只會匯出
  有 `dllexport` 的符號（曾導致 `LIBUSB_ERROR_ACCESS`）。
* `src/helper.c`：libusb RPC daemon；bulk 使用非同步 `libusb_submit_transfer`，
  不做 200 ms 切片、不重送被取消的 URB。
* `scripts/gen_def.py`：從 Wine 的 `setupapi.dll` 自動產生 forwarder 清單。

## 目前狀態

| 項目 | 狀態 |
|---|---|
| 列舉、`CreateFile`、`WinUsb_Initialize` | ✅ |
| READY、波形、連續 30 秒擷取 | ✅ |
| `Stop` → 再 `Run` | ✅（2026-10-30 三次循環驗證） |
| FPGA bitstream 上傳 | ❌ 傳輸成功但裝置未設定（需 Windows VM） |

## 建置與安裝

```bash
./scripts/build.sh                 # 需要 mingw-w64、libusb-1.0、python3
WINEPREFIX=~/.wine-dswave ./scripts/install.sh
WINEPREFIX=~/.wine-dswave wine "C:\\Program Files\\DSWave\\DSWave.exe" -l 5
WINEPREFIX=~/.wine-dswave ./scripts/uninstall.sh   # 還原
```

`WINE_LIB` 預設 `/opt/wine-devel/lib/wine/x86_64-windows`，可覆寫。

日誌：

* `/tmp/dswave-helper.log`
* `C:\ProgramData\DSWaveShim\{setupapi,winusb,cancelio}.log`

## 環境變數

| 變數 | 預設 | 說明 |
|---|---|---|
| `DSW_DEBUG` | 0 | 詳細日誌 |
| `DSW_MBPS` | 0 | 讀取速率上限（MB/s） |
| `DSW_CHUNK_BYTES` | 405504 | bulk IN 大小 |
| `DSW_FILL` | 1 | 串接封包填滿緩衝區 |
| `DSW_NO_RESET` | 0 | 開啟時不做 reset |
| `DSW_LIST_LIVE` | 0 | 關閉 descriptor 快取 |
| `DSW_SOFT_RESET` | 0 | 關閉時重放 stop 序列 |
| `DSW_FPGA_RECOVER` | 0 | 上傳前 reset + 重放序列 |
| `DSW_CANCELIO_HOOK` | 1 | 啟用 CancelIoEx hook |

## 已知限制

* 每筆 bulk 傳輸多一次 loopback TCP hop；低速率可用，全速需改 unixlib。
* `setupapi=native` 會讓整個 prefix 都使用墊片（其餘呼叫已 forward）。
* FPGA bitstream 上傳在 Wine 下尚未成功；每次斷電後需用 Windows VM
  設定一次。
