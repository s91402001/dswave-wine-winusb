# DSWave on Wine + WinUSB — 進度紀錄

最後更新：2026-10-29

## 目標

在 Linux（Ubuntu 24.04）上以 Wine 執行 `DSWave_v1.1.0_x64_setup.exe`，
並透過 WinUSB 與裝置通訊：

- 裝置：`2a0e:0080` Shenzhen DreamSource Technology / "USB-based DSL Instrument v3"
- 位置：Bus 008 Device 006（USB 3.0，5000M）
- 介面：1 個 vendor-specific interface（class `ff/00/00`）

成功門檻（使用者決定）：**能列出裝置、低速率可用**（不追求全速）。

---

## 環境現況

| 項目 | 值 |
|---|---|
| OS | Ubuntu 24.04.5 LTS，kernel 7.0.0-38-generic |
| Wine | 11.20（wine-devel / wine-staging 都有，預設 `/opt/wine-devel`） |
| 測試 prefix | `~/.wine-dswave`（win64，DSWave 1.1.0 已安裝） |
| udev | `/etc/udev/rules.d/60-dreamsourcelab.rules`（`2a0e` → 0666）已存在 |
| VM | `win11`（libvirt）目前關機；裝置已回到 host |
| 原始 exe | `~/Documents/DreamSourceLab/DSWave_v1.1.0_x64_setup.exe` |
| 墊片專案 | `~/Documents/DreamSourceLab/wine-winusb-shim/` |

---

## 關鍵結論（已證實）

1. **Wine 11.20 的 `winusb.dll` 全部是 stub**
   - `winusb.dll` 21 個 `WinUsb_*` 匯出都是 `__wine_stub_*`；上游
     `dlls/winusb/main.c` 只有 `FIXME("stub")`。
   - 因此任何 Windows 程式在 Wine 下都無法用 WinUSB 做 I/O。

2. **Wine 沒有 USB function driver**
   - `wineusb.sys` 只是 bus driver：建立 `\Device\USBPDO-N`，只處理 kernel 的
     `IRP_MJ_INTERNAL_DEVICE_CONTROL`，沒有 user-mode 可開啟的裝置、也沒有註冊
     `GUID_DEVINTERFACE_WINUSB`。
   - 所以 `SetupDiGetClassDevs(WINUSB)` 回 0 個裝置（自製 `usbscan.exe` 實測）。

3. **DSWave 的 USB 層是 libusbK 的 libusb-1.0 包裝**
   - `DSWave.exe` 靜態連結 libusb/libusbK 程式碼（字串：`libusbK DLL found`,
     `LUsb0`, `BSP: Failed to security check of usb device`, `WinUSB-like APIs`）。
   - 匯入表只有 4 個 SetupAPI 函式，其餘以 **`GetProcAddress` 動態解析**。
   - 以**絕對路徑**載入：
     `C:\windows\system32\SetupAPI.dll`、`C:\windows\system32\WinUSB.dll`，
     找不到時再試 `C:\windows\system32\libusbK.dll`（不存在）。
   - 因此只把 DLL 放在程式目錄會被 Wine 的 builtin 機制忽略，
     **必須覆蓋 system32 並設 `DllOverrides=native`**。

4. **沒有原生 Linux 替代**
   - DSView 1.3.2 只支援 PID `0x0001`–`0x002D`；本機是 `0x0080`，不在表內。
   - DSWave 為閉源、官方只出 Windows 版。

5. **裝置會被 VM 佔用**
   - virt-manager 以 SPICE USB 轉送持有 `/dev/bus/usb/008/006`；
     測試前必須關 VM／取消轉送。

---

## 已完成的工作

### 專案結構

```
wine-winusb-shim/
├── README.md
├── src/
│   ├── proto.h            # TCP RPC 協定
│   ├── helper.c           # 原生 Linux libusb daemon（每連線一個 thread）
│   ├── setupapi_shim.c    # setupapi.dll 墊片（合成 WinUSB 裝置）
│   └── winusb_shim.c      # winusb.dll 墊片（21 個 WinUsb_*）
├── scripts/
│   ├── gen_def.py         # 從 Wine setupapi.dll 產生 597 個 forwarder
│   ├── build.sh
│   ├── install.sh         # 覆蓋 system32 + DllOverrides + 啟動 helper
│   └── uninstall.sh       # 還原
└── build/                 # helper / setupapi.dll / winusb.dll
```

### 架構

```
DSWave.exe (libusbK/libusb 靜態連結)
  ├─ SetupDi*  ── setupapi.dll shim ── 合成 2a0e:0080
  │                                   路徑：\\?\C:\ProgramData\DSWaveShim\
  │                                          USB#VID_2A0E&PID_0080#8&6#{A5DCBF10-...}
  │                                   （指向真實空檔案，CreateFile 可開啟）
  └─ WinUsb_*  ── winusb.dll shim ── TCP 127.0.0.1:47123
                                       └─ helper（libusb）── /dev/bus/usb/008/006
```

- `winusb.dll` 用 `GetFinalPathNameByHandleW` 從路徑反查 bus/addr，
  再向 helper 開裝置；bulk/control/descriptor 皆已實作，overlapped 以
  同步完成 + `SetEvent` 處理。
- `setupapi.dll` 只攔 USB 相關列舉，其餘匯出以 `.def` forward 回
  `setupapi_orig.dll`。

### 已補上的裝置屬性（DSWave 會查的）

- `SPDRP_INSTALL_STATE(0x22)` = 0（installed）
- `SPDRP_SERVICE` = `WinUSB`
- `SPDRP_UPPERFILTERS` / `SPDRP_LOWERFILTERS` = 空 multi-sz
- `SPDRP_DRIVER` = `{36fc9e60-...}\0001`
- `SPDRP_ADDRESS`、`SPDRP_REMOVAL_POLICY`、`SPDRP_HARDWAREID`、
  `SPDRP_COMPATIBLEIDS`、`SPDRP_CLASSGUID`、`SPDRP_CLASS`、`SPDRP_DEVICEDESC`
- Enum key（`Enum\USB\VID_2A0E&PID_0080\...`）與 Class key
  （`Control\Class\{36fc9e60-...}\0001`）的標準值

---

## 驗證結果

### ✅ 端到端測試（`usbscan.exe`，模擬 libusb 行為）

```
=== GUID_DEVINTERFACE_WINUSB ===
  [0] path: \\?\C:\ProgramData\DSWaveShim\USB#VID_2A0E&PID_0080#8&6#{A5DCBF10-...}
       CreateFile OK, WinUsb_Initialize -> 1
       WinUsb_QueryInterfaceSettings -> 1, numEndpoints=3
=== GUID_DEVINTERFACE_USB_DEVICE ===
  同上
```

helper 端也確認：`2a0e:0080 at 8:6 speed 3`，可 `libusb_open`、讀 device descriptor。

### ✅ DSWave 安裝與啟動

- `MSI/NSIS /S` 安裝成功（`C:\Program Files\DSWave`）
- Qt 6.8.0、UI 正常、Demo 模式可操作（截圖 `/tmp/opencode/dswave-ui.png`）

### ✅ DSWave 掃描已打到墊片

以 `setupapi=native` 後，`GetProcAddress("SetupDiGetClassDevsA")` 解析到墊片，
掃描流程（節錄自 `C:\ProgramData\DSWaveShim\setupapi.log`）：

```
GetClassDevsA(NULL,NULL,flags=0x16)          → 合成裝置 2a0e:0080 8:6
EnumDeviceInterfaces guid=f18a0e88            → 無
EnumDeviceInterfaces guid=a5dcbf10            → 1 個（我們的裝置）
GetDeviceInterfaceDetailA size=0 → 198        → 取得路徑
GetDeviceInstanceIdA
GetDeviceRegistryPropertyA prop=0x22/0x4/0x11/0x12
EnumDeviceInterfaces guid=3abf6f2d            → 無
GetClassDevsA(NULL,"USB",flags=0x6)           → 合成裝置
GetInstanceIdA / GetDeviceRegistryPropertyA prop=0x9
OpenDevRegKey key_type=1                      → Enum key
EnumDeviceInterfaces guid=4d1e55b2 (HID)      → 無
```

### ❌ 尚未通過：DSWave 沒有開啟裝置

- 掃描結束後 **沒有 `CreateFile` 裝置路徑、也沒有 `WinUsb_Initialize`**。
- UI 仍只有「Demo」，沒有出現實體裝置。
- 一度在 relay log 看到 `CreateFile` 我們的合成路徑，後來確認為
  **shim 自己 `ensure_file()` 的呼叫**，不是 DSWave。

### 推測的卡點

1. libusbK BSP 的 driver/security 檢查（`BSP: Failed to security check of usb device`）。
2. 應用層用 PID/profile 對照表過濾（`ERROR: can't find device's profile`）。
3. 需要額外的 registry 值或 IOCTL 探測回應。

---

## 如何重現

```bash
cd ~/Documents/DreamSourceLab/wine-winusb-shim
./scripts/build.sh
WINEPREFIX=~/.wine-dswave ./scripts/install.sh      # helper 會啟動

WINEPREFIX=~/.wine-dswave wine "C:\\Program Files\\DSWave\\DSWave.exe" -l 5

WINEPREFIX=~/.wine-dswave ./scripts/uninstall.sh     # 還原
```

- 墊片日誌：`~/.wine-dswave/drive_c/ProgramData/DSWaveShim/{setupapi,winusb}.log`
- helper 日誌：`/tmp/dswave-helper.log`
- DSWave 日誌等級：`-l 5`（0–5）

---

## 目前的系統狀態

- `~/.wine-dswave` 已套用墊片（`HKCU\Software\Wine\DllOverrides`：
  `setupapi=native`, `winusb=native`；system32 已備份為
  `setupapi_wine.dll` / `winusb_wine.dll`，另有 `setupapi_orig.dll`）
- helper 執行中（`pgrep -a helper`）
- 裝置在 host、VM 關機中

---

## 下一步建議

1. **定位拒絕點**：用 `winedbg` 對 `SetupDiOpenDevRegKey` 或掃描函式下斷點，
   往上追哪個條件 return；或在 `usbscan` 擴充成完整模仿 libusbK 的呼叫序列比對。
2. **補齊 registry 值**：依真實 Windows 上 `WinUSB device` 的 Enum/Class key
   補上缺少的機碼值。
3. **若確認需要 IOCTL**：在 shim 內加 `DeviceIoControl` 回應（需先確定 code）。
4. **備案**：回到 VM 並改用 `usb-host` hostdev（qemu-xhci）直通，避開 Wine。

### 風險 / 限制

- 每筆 bulk 傳輸多一次 loopback TCP hop；低速率可接受，全速需改 Wine unixlib。
- `setupapi=native` 會讓整個 prefix 都走墊片（其餘呼叫已 forward）。
- Wine 升級後需重跑 `install.sh`。

---

## 2026-10-29 下午更新：假 PnP 樹實作完成，裝置已可在 Wine 內列出

### 已完成
- `cfgmgr32` 墊片（`CM_Get_Parent` / `CM_Get_Child`）
- `setupapi` 三裝置表（HCD `0x3000` / HUB `0x2000` / DEV `0x1000`）
- `DeviceIoControl` IAT hook（`0x220408` / `0x220448` / `0x22045c` / `0x220410`）
- helper `REQ_LIST` 直接帶回 device/config descriptor（不需 claim）
- helper langid 修正（string descriptor 可讀）
- helper per-connection handle 所有權（斷線自動釋放）
- 修正 OUT 傳輸誤收 payload 的協定 bug

### 驗證結果
```
libusb: (bus: 1, addr: 6, depth: 1, port: 6): 'USB\VID_2A0E&PID_0080\8&6'
hal: Device (DSCope M454) status: INIT → OPEN → READY
hal: Fpga is already done (DSCope M454)
hal: MSO ZERO: Zero info loaded successfully.
hal: model: received event HAL_EVENT_DEVICE_CONNECTED.
```
UI 出現第二個分頁 **「H0: DSCope M454」**，狀態列顯示
`DSCope M454 OSC 100US 100MΩ 1M 200MΩ 2M`。

### 尚未通過：擷取
- 按下執行後，libusb 對 bulk IN `0x86` 讀 4096 bytes：
  - 第一次回 `LIBUSB_ERROR_IO`（stall）
  - `libusb_clear_halt` 後重試，60 秒 `LIBUSB_ERROR_TIMEOUT`
- 裝置無資料輸出 → `HAL_EVENT_DEVICE_ERROR`
- 尚未確認是否需要額外的硬體啟動命令、或非同步 submit 語意、
  或應改用 `0x84` endpoint

### 目前檔案
- `implementation_plan.md`（實作計畫）
- `wine-winusb-shim/`（三顆墊片 + helper + 腳本）
- 日誌：`C:\ProgramData\DSWaveShim\{setupapi,cfgmgr32,winusb}.log`、
  `/tmp/dswave-helper.log`

---

## 2026-10-29 15:05 更新：Stage 4 打通，擷取可執行

### 完成
- `winusb_shim.c` 改為真正的 OVERLAPPED 非同步：
  - `WinUsb_ReadPipe/WritePipe` 立即回 `FALSE + ERROR_IO_PENDING`
  - 8 條 worker pool，每條使用自己的 TCP 連線
  - `Internal/InternalHigh` 由 worker 填寫後 `SetEvent`
  - `WinUsb_AbortPipe` 只標記 in-flight（避免 UAF）、送 `REQ_ABORT_PIPE`
  - `WinUsb_ResetPipe` → `REQ_RESET_PIPE`
  - `WinUsb_GetOverlappedResult` 支援 PENDING / TIMEOUT / CANCELLED
- `helper.c`：
  - 鎖只保護 handle 查表；transfer 不再持鎖
  - per-`(handle, endpoint)` FIFO 鎖
  - 200 ms 切片 + 部分讀取累積
  - abort generation counter
  - handle refcount，關閉時自動 abort 進行中的 transfer
- 修正 `SHIM_MAX_XFER` 4 MiB → 64 MiB：
  DSWave 會提交 **5,005,312 bytes** 的 bulk IN，原本被墊片拒絕。

### 驗證
```
hal: TransferThread: Start acquire for DSCope M454
hal: BSP: Send start command to hardware.
hal: model: received event HAL_EVENT_DEVICE_RUNNING.
helper: bulk done ep=86 status=0 total=4096
helper: bulk done ep=86 status=0 total=5005312
helper: bulk done ep=86 status=0 total=5005312
```
UI 進入執行狀態（播放鍵變紅色停止鍵），波形區顯示「等待信號…」
（目前沒有接訊號源，屬正常）。

### 備註
- 控制傳輸在 bulk 等待期間仍可即時執行（實測 1 ms）
- helper 端 abort 實測 0.6 秒內結束
- UI 自動化點擊不穩定；測試時需確認裝置分頁已就緒後再按執行

---

## 2026-10-29 15:58 更新：動態位址與 IOCP 完成通知

### 修正
1. **動態裝置位址**：裝置擷取後會重新列舉（8:6 → 8:7 → 8:8）。
   現在 `setupapi` 每次列舉都向 helper 取目前 bus/addr，
   動態組出 instance id、路徑、ADDRESS、LOCATION；helper 的 `REQ_OPEN`
   在精確位址找不到時會 fallback 到同 VID/PID 的裝置。
2. **I/O Completion Port**：libusb 的 Windows 後端用 IOCP 接收非同步完成
   （`CreateIoCompletionPort` + `GetQueuedCompletionStatus`）。
   我們的 worker 只設 `OVERLAPPED` 與 `SetEvent`，libusb 永遠收不到完成 →
   App 卡在「等待渲染」。
   現在 IAT hook `CreateIoCompletionPort` 記錄 handle → (iocp, key)，
   worker 完成後呼叫 `PostQueuedCompletionStatus`，libusb 即可正常完成。

### 驗證
```
hal: TransferThread: Start acquire for DSCope M454
hal: model: received event HAL_EVENT_DEVICE_RUNNING.
helper: bulk data ep=86 len=5005312 head=71 71 71 71 71 72 71 71 ...
helper: bulk done ep=84 status=0 total=1
```
- 0x84 狀態讀取成功、5MB 資料傳輸完成、libusb 收到完成事件。

### 尚未通過
App 在 `msopackethandler.cpp:266` 觸發 assert：
```
m_mso.m_pkt_bytes <= buf_bytes - head_bytes
```
代表封包切分/對齊與 App 期待的不一致（目前觀察：4096-byte 標頭
`55 55 55 55 02 00 00 00 ...`，之後 5,005,312 bytes 資料，
再下一次讀取只回 327,680 bytes 且開頭是取樣資料）。
下一步：解碼 4096-byte 標頭的長度欄位，確認 App 的讀取大小是否
與裝置封包大小一致（疑似差 4096 bytes 造成漂移）。

---

## 2026-10-29 16:05 現況總覽（最新）

### 現在可以做到
1. 在 Wine 開啟 DSWave（`~/.wine-dswave`）
2. 自動列出並開啟 `2a0e:0080`，辨識為 **DSCope M454**
3. 完成 FPGA / ADC / 校正初始化，狀態進入 `READY`
4. 按下執行後進入 `DEVICE_RUNNING`，libusb 可正確收到 bulk 完成
5. 實際從 0x86 收到資料（例：5,005,312 bytes，內容為取樣值）
6. 0x84 的 1-byte 狀態讀取成功

### 現在還不行
- **波形沒有渲染**：App 在 `msopackethandler.cpp:266` 觸發
  `m_mso.m_pkt_bytes <= buf_bytes - head_bytes` assert，之後執行緒卡死。
- 判定為 **USB 讀取大小與裝置封包長度未對齊**，不是傳輸失敗。

觀察到的序列：
| 讀取 | 大小 | 開頭 |
|---|---|---|
| 1 | 4,096 | `55 55 55 55 02 00 00 00 00 00 00 80 …`（標頭） |
| 2 | 5,005,312 | 取樣資料 |
| 3 | 327,680 | 取樣資料（短讀） |

### 本日累積修正
| 項目 | 內容 |
|---|---|
| 假 PnP 樹 | HCD `0x3000` / HUB `0x2000` / DEV `0x1000` |
| `cfgmgr32` | `CM_Get_Parent` / `CM_Get_Child` 父子關係 |
| Hub IOCTL | `0x220408` / `0x220448` / `0x22045c` / `0x220410` |
| 非同步 bulk | OVERLAPPED + worker pool + 立即 `ERROR_IO_PENDING` |
| Abort/Reset | generation counter、per-endpoint FIFO、200 ms 切片 |
| **動態位址** | 裝置重列舉（8:6→8:7→8:8）後仍可開啟 |
| **IOCP 完成** | `CreateIoCompletionPort` hook + `PostQueuedCompletionStatus` |
| 大封包 | `SHIM_MAX_XFER` 4 MiB → 64 MiB（App 會送 5,005,312 bytes） |

### 啟動方式
```bash
pgrep -x helper >/dev/null || (cd ~/Documents/DreamSourceLab/wine-winusb-shim && \
  setsid nohup ./build/helper >/tmp/dswave-helper.log 2>&1 </dev/null &)
WINEPREFIX=~/.wine-dswave wine "C:\\Program Files\\DSWave\\DSWave.exe" -l 5
```

### 日誌位置
- `~/.wine-dswave/drive_c/ProgramData/DSWaveShim/winusb.log`
- `~/.wine-dswave/drive_c/ProgramData/DSWaveShim/setupapi.log`
- `~/.wine-dswave/drive_c/ProgramData/DSWaveShim/cfgmgr32.log`
- `/tmp/dswave-helper.log`

### 下一步
1. dump 4096-byte 標頭的完整內容（64+ bytes），解出 `m_pkt_bytes` 欄位
2. 比對「標頭宣告 payload 長度」與 App 請求的 5,005,312 是否差 4,096
3. 依結果調整 helper 的讀取策略（一個封包一次回傳，不合併、不跨封包）
4. 重測波形渲染

---

## 2026-10-29 16:25 更新：封包對齊診斷（P0 / P1 執行結果）

### 已做的量測
在 `winusb_shim.c` 與 `helper.c` 加入：
- 每次 bulk 提交的 `seq`（每個 `(handle, endpoint)` 遞增）
- `submit` / `worker start` / `worker post`（含 bytes、status）
- 每個 IN 傳輸的 `head64`、`tail64`、累計 `stream offset`
- 4096-byte 封包的所有非零 32-bit word
- 掃描 `0x55555555` 魔數的絕對位置

### 結果 1：完成順序正常（排除「亂序」假設）
```
submit seq=0 pipe=86 len=4096
submit seq=1 pipe=86 len=5005312
submit seq=2 pipe=86 len=5005312
worker post seq=0 bytes=4096    status=OK
worker post seq=1 bytes=5005312 status=OK
worker post seq=2 bytes=5005312 status=OK
```
- 完成順序 = 提交順序
- `post` 的 bytes = 實際收到長度
→ 不需 ticket lock，也不是 IOCP 長度錯誤。

### 結果 2：4096-byte 標頭沒有長度欄位
```
off=0  val=55555555
off=4  val=00000002
off=8  val=80000000
（offset 12 之後到 4095 全為 0）
```
→ `m_pkt_bytes` 不是從這個標頭讀出來的，而是 App 依取樣參數自行計算。

### 結果 3：裝置實際送出的長度不足（關鍵）
```
helper: bulk start seq=1 ep=86 len=5005312
helper: bulk data  seq=1 ep=86 len=2949120   ← 只收到 2,949,120
helper: bulk done  seq=1 ep=86 status=0 total=2949120
helper: bulk start seq=2 ep=86 len=5005312
helper: bulk done  seq=2 ep=86 status=-1    ← 之後回 IO 錯誤
```
- App 期待 5,005,312 bytes，裝置只送 2,949,120 bytes（ZLP 結束）
- App 接著收到短封包 → `m_mso.m_pkt_bytes <= buf_bytes - head_bytes` assert
- 使用者看到的錯誤對話框即為此 assert

### 判讀
- **不是** 傳輸失敗、不是完成亂序、不是 IOCP 長度錯誤
- 是 **裝置實際產生的影格大小與 App 依參數期待的 5,005,312 不一致**
- 最可能是取樣率／深度／模式的控制命令沒有正確套用到硬體
  （我們目前只是原樣轉送 b0/b1/b2 控制命令，尚未與正常 Windows 行為比對）

### 已做的程式調整
- `helper.c`：短封包（short packet / ZLP）時不再立即結束，會繼續累積到
  請求長度或 deadline（仍保留 `xfer == 0` 的退出條件）
- 所有診斷日誌保留，方便與 Windows 端錄製比對

### 下一步（決定性）
1. 在 Windows 11 VM 用 **USBPcap + Wireshark** 錄一段正常擷取
2. 比對：
   - `b0/b1/b2` 控制命令的實際 bytes
   - 每次 bulk IN 的請求長度與實際完成長度
   - 第一個資料封包的長度（5,005,312 或 2,949,120）
3. 依差異修正控制命令或讀取策略，再重測波形

### 目前裝置狀態
- `Bus 008 Device 008`（未被 VM 佔用）
- 三顆墊片已安裝、helper 執行中
- App 按下執行後會收到資料，但最後仍會觸發上述 assert

---

## 2026-10-29 17:25 Windows USBPcap 基準分析

### 擷取
- `output.pcap`（129 MB、7,881 packets、41.9 秒），USBPcap 格式
- 解析工具：`usbpcap/linux/parse-usbpcap.py`（不需 tshark）

### Windows 行為
```
4096-byte 標頭 ×1        head = 55 55 55 55 01 00 00 00 00 00 00 80
0x86 bulk IN  405,504 bytes ×1964
0x84 bulk IN        1 byte  ×246
非零狀態 bulk: 0
```

### Wine 行為（相同預設設定）
```
4096-byte 標頭 ×1        head = 55 55 55 55 02 00 00 00 00 00 00 80
0x86 bulk IN  5,005,312 bytes ×N（偶爾 2.4–3.4MB 後 IO 錯誤）
0x84 曾失敗
```

### 關鍵差異
| 項目 | Windows | Wine |
|---|---|---|
| 標頭 type（offset 4） | **1** | **2** |
| 每次讀取大小 | 405,504 | 5,005,312 |
| 錯誤 | 0 | 多次 |

### 執行開始的控制命令差異（相似度 75.7%）
```
Windows: 40 0d 00 / 28 00 04 / 2b 00 00（通道 0x50/0x60/0x70 都是 0x0400）
Wine:    40 05 00 / 28 02 01 / 2b 00 00（0x0102 / 0x0201 / 0x0300）
```

### 已排除
- 傳輸順序、IOCP 長度、200ms 切片（timeouts=0 仍失敗）
- 核心 USB 錯誤（dmesg 乾淨）
- Wine profile 狀態：刪除 `AppData\Roaming\DreamSourceLab\DSWave` 後重跑，
  標頭仍為 type 2、讀取仍為 5,005,312

### 待確認
- 標頭 type 1/2 的意義（推測與擷取模式/通道組成有關）
- Windows 端是否為 USB3 SuperSpeed（config descriptor 有 SS companion，應該相同）
- 需要 Windows 端「同設定、SuperSpeed」的第二次擷取，或原廠封包規格

---

## 2026-10-29 17:32 突破：波形成功渲染

### 根因（由 Windows USBPcap 基準比對得出）
- 裝置的 bulk IN **天然封包大小是 405,504 bytes**（Windows 上每次 URB 都是這個尺寸）
- App 在 Wine 上卻一次要求 5,005,312 bytes
- 我們的 helper 原本一次 `libusb_bulk_transfer(5MB)` → 裝置在 405,504 bytes
  就結束（ZLP），回報短封包 → App 的 packet handler assert
  （`m_pkt_bytes <= buf_bytes - head_bytes`）

### 修正（`helper.c`，已設為預設）
1. `DSW_CHUNK_BYTES` 預設 **405,504**：每次 `libusb_bulk_transfer` 只抓一個裝置封包
2. `DSW_FILL` 預設 **開啟**：短封包後繼續讀，累積到呼叫端要求的長度
3. 結果：5,005,312 bytes 由 13 個 405,504-byte 封包組成，**零逾時、零 IO 錯誤**

```
helper: bulk done seq=1 ep=86 status=0 total=5005312 slices=13 timeouts=0 why=short
helper: bulk done seq=2 ep=86 status=0 total=5005312 slices=13 timeouts=0 why=short
```

### 驗證
- **assert 消失**（`wassert` 次數 = 0）
- **波形正常渲染**：D0/D1/D2 方波、A2/A3 雜訊（截圖 `/tmp/opencode/def1.png`）
- 連續兩次完整 5,005,312 bytes 傳輸成功

### 仍待處理
- 約 2 個影格後，裝置回 `LIBUSB_ERROR_STALL`（seq4 在 3,244,032 bytes 後中斷，
  seq5 直接 STALL），App 顯示 `LIBUSB_TRANSFER_STALL` 並停止。
  波形已顯示，但長時間擷取仍會中斷。
- 可能方向：App 停止時的 abort 與裝置 STALL 的處理、或讀取節奏需要再調整。

### 可調環境變數
- `DSW_CHUNK_BYTES`（預設 405504）
- `DSW_FILL`（預設 1）
- `DSW_SLICE_MS`（預設 200）

---

## 2026-10-29 17:53 預設 chunk 改為 811,008 並驗證

### 變更
- `helper.c` 預設 `chunk` 由 405,504 改為 **811,008**（環境變數 `DSW_CHUNK_BYTES` 仍可覆寫）
- `DSW_FILL` 預設維持開啟

### 驗證
```
第一影格: xfer=811008 ×6 + xfer=139264 ×1 → total=5005312 slices=7
seq=4 total=5005312 slices=7
seq=5 total=5005312 slices=7
```
- 每影格由 13 個切片降為 **7 個切片**
- **assert = 0**
- **波形正常渲染**（D0/D1/D2 方波、A2/A3 雜訊，截圖 `/tmp/opencode/def811b.png`）

### 停止 / abort 驗證
- chunk 811,008 測試中，待處理的讀取在停止時以
  `status=-10 (LIBUSB_ERROR_INTERRUPTED)` 結束，代表 `AbortPipe` → `REQ_ABORT_PIPE` 生效
- 延遲在下一個切片邊界（≤400 ms）

### 仍待處理
- 約 3 個影格後裝置停止送資料（seq6 收到 2,146,304 bytes 後 IO 錯誤，seq7 直接失敗）
- 判讀為「沒有訊號可觸發下一次影格」或裝置在影格邊界的行為
- 接上訊號源後應可驗證連續擷取

---

## 2026-10-29 18:18 穩定性修正：log 降量 + 開機重置

### 變更
1. **日誌降量**：`helper.c` 預設只記錄摘要（bulk start/done、錯誤、abort）。
   詳細的 `slice` / `head64` / `tail64` / word dump 需 `DSW_DEBUG=1`。
2. **第一次開啟時重置裝置**：`libusb_reset_device()`（每個 helper 程序一次，
   `DSW_NO_RESET=1` 可停用）。用於清除前次失敗留下的 halted endpoint。
3. 預設 chunk 維持 **811,008**。

### 驗證
```
full frames: 4
errors: 0
assert: 0
```
- 裝置成功開啟（READY 2 秒內）
- **連續 4 個完整 5,005,312-byte 影格，零錯誤**
- 波形正常渲染（D0/D1/D2 方波、A2/A3 雜訊，截圖 `/tmp/opencode/final.png`）
- 先前「2–3 個影格後 IO 錯誤」的現象**已消失**

### 仍待處理
- 4 個影格後裝置停止送資料，但**沒有錯誤**（只是等待下一次觸發）
- 接上連續訊號並確認觸發源（A0 或 D0）後，應可持續擷取

### 新增環境變數
- `DSW_DEBUG=1`：詳細日誌
- `DSW_NO_RESET=1`：停用開啟時重置
- `DSW_CHUNK_BYTES` / `DSW_FILL` / `DSW_SLICE_MS` 仍可覆寫

---

## 2026-10-30 資料流穩定性：EPROTO 與裝置卡死

### 目前結論
- Wine 下可以開啟裝置、進入 READY、顯示波形，但資料流會在 **0.3～1.1 秒**
  （30～95 MB、6～19 個影格）後中斷，libusb 回報 **-EPROTO**（urb status=-71）。
- 中斷後 App 顯示 `usb device not ready`，第二次 Run 必定跳出
  「系統發現不穩定的usb連接」警告。
- 中斷後裝置進入「有 4096 header、沒有 frame 資料」的卡死狀態：
  - 重開 App 無效（`Fpga is already done` 會跳過 FPGA 設定）
  - USB reset + 重放 SCPI 設定也無效（不確定性成功）
  - 只有 **重新上傳 FPGA bitstream（Windows VM）或重新插拔** 能恢復

### Windows 基準（fpga1.pcap，2026-10-30 09:53）
- 2,818 packets / 13.47 s / 8.9 MB（snaplen 65,535）
- FPGA 上傳：bulk OUT `0x02`，**466,408 bytes 一次完成、status 0**，
  接著 `0x02` IN len=0
- 擷取資料：`0x86` IN **405,504 bytes × 133**，間隔約 22 ms（**18.6 MB/s**）
  - 開頭先讀 4096 bytes header（`55 55 55 55 01 00 00 00`）
- 停止時：pending read 以 **status=0xC0010000 (USBD_STATUS_CANCELED)、len=0** 結束
- 控制序列
  - Start：`03 01 00` → `02 00 00` → `01 01 00`
  - Stop ：`01 00 00` → `02 08 00` → (0x84 out0/in1 ×3) → `01 00 00` → `02 04 00`
- 每次 0x86 IN 之前都有一筆 `0x86 out len=0`（USBPcap 記為 out，實際應為 ZLP/no-op）
- 設定（截圖）：TimeBase 2ms、400Kpts、Analog 5MSa/s、Digital 100MSa/s
- 限制：snaplen 65,535 導致 466,408 bytes bitstream **只存到 65,508 bytes**，
  無法用來在 Linux 重放 FPGA 上傳

### Wine 現況（2026-10-30 13:20 最後一次）
```
ctl 03 01 00 / 02 00 00 / 01 01 00
header 4096 -> OK
seq=1  total=335,872  -> stall ret=-1 (EPROTO)
app: Data transfer failed: LIBUSB_SUCCESS / LIBUSB_TRANSFER_COMPLETED; length: 0
app: usb device is not ready: ERROR
```
- Stop 之後 App **沒有送出 `02 04 00`**（helper log 計數 = 0）
- `01 00 00`（Stop）→ helper 會中止 pending IN，UI 可以正常停止

### 獨立驗證（不經 App、不經 Wine）
工具：`/tmp/dswbulk`（直接 libusb，重放 App 的 1,202 筆 SCPI 設定）
- 同一台裝置、`0x86`、405,504 bytes、20 MB/s、5 秒
  → **170 MB / 34 影格 / 零錯誤**
- 結論：**裝置與 USB 連結正常**，問題在 App 與 shim 的互動（時序／狀態碼）
- 但在任何 STOP（`01 00 00`）或 URB 取消之後，裝置會卡死，需重新設定 FPGA

### 已嘗試的修正與結果
| 修正 | 結果 |
|---|---|
| mid-transfer stall 立即回傳（不再等 300s deadline） | Stop 不再卡死（保留） |
| 收到 Stop (`01 00 00`) 時中止 pending IN | UI 可立即停止（保留） |
| `DSW_MBPS=20` pacing | 仍會 stall（只是撐久一點） |
| `DSW_SLICE_MS=2000`（減少 URB 取消） | 仍會 stall；卡死時只收到 header |
| chunk 405,504（對齊 Windows） | 仍會 stall |
| 中止/EPROTO 回報 `0xC0010000` | App 解讀成 `LIBUSB_SUCCESS / COMPLETED; length 0`，仍未送 `02 04 00` |

### 待釐清
1. `WinUsb_GetOverlappedResult` 目前只處理 `STATUS_SUCCESS / STATUS_IO_TIMEOUT /
   STATUS_CANCELLED`，`0xC0010000` 落到 default（`ERROR_GEN_FAILURE`）；
   但 App 記錄成 SUCCESS，需確認 App 實際用哪個 API 取狀態。
2. EPROTO 的真正觸發點：裝置在影格中途停止送資料（有一次 200ms 無資料），
   取消 URB 後重送即 EPROTO。
3. 缺少 `02 04 00` 是否就是裝置卡死的主因（Windows 停止序列的最後一步）。
4. Windows 若連續擷取超過 ~95 MB 是否也會 stall（目前 pcap 只有 2.9 秒 / 54 MB）。

### 下一步
1. 修正 `WinUsb_GetOverlappedResult` 的狀態映射，並在 Stop 時補送 `02 04 00`
   （讓裝置回到可重新啟動的閒置狀態）。
2. 用更大 snaplen 重錄 Windows pcap（含 60 秒連續擷取），確認長時間行為並取得
   完整 bitstream，供 Wine 下的 FPGA 上傳重放。
3. 重新評估 URB 取消策略：不主動取消（slice = App timeout），只在 Stop 時依
   generation 中止。

### 目前裝置狀態
- `Bus 008 Device 015`，FPGA **已由 VM 設定**（2026-10-30 13:20 之後）
- 最後一次測試後裝置處於**卡死狀態**：需要重新設定 FPGA（VM）或重新插拔才能再測

---

## 2026-10-30 下午 第二階段：EPROTO 已解、Stop 仍會卡死

### 已完成並驗證
1. **P0-1 descriptor 快取 + 2 秒節流**
   - `REQ_LIST` 造成的 EP0 `GET_DESCRIPTOR`：1,627 次 → 6 次
2. **P0-2 非同步 bulk 傳輸**
   - 移除 200 ms 切片，改用 `libusb_submit_transfer` + event loop
   - 只在 abort 時取消一次，且等 callback 回來才釋放（防 UAF）
3. **結果：連續 30 秒擷取成功、無 `-EPROTO`**
   - 證實 EPROTO 根因＝「熱插拔 EP0 流量」＋「URB 反覆取消」

### Stop 卡死（未解）
14:10 測試證據：
- `cancelio.log` 不存在 → `CancelIoEx` hook 沒有被命中
- `winusb.log` 沒有 `AbortPipe`、沒有 `Free handle`
- 同時有 3 筆 5 MB 讀取在飛（seq 27/28/29）；27 完成，28/29 未 post
- helper：seq=28 回 `-10`（4.5 MB partial）、`d=001d` 回 `-2`（handle 已關閉）
- 結論：Stop 時 App 關閉裝置/連線，pending 讀取的 completion 沒有送出，
  App 永遠等不到 → 沒有回應

### 已修正的 regression
- `__declspec(dllexport)` 讓 mingw 停止自動匯出其他符號，
  `winusb.dll` 只剩 1 個 export → App 無法呼叫 `WinUsb_*` → `LIBUSB_ERROR_ACCESS`
- 修正：`build.sh` 加 `-Wl,--export-all-symbols`
  （已驗證：20 個 `WinUsb_*` + `shim_cancel_overlapped` 都可取得）

### 下一步
1. 加掛 `CloseHandle` hook：關閉 shim 檔案時，對該 ctx 所有 in-flight bulk
   直接以 `STATUS_CANCELLED` 完成（不等 helper）
2. worker 在 socket 失敗時必須保證 post completion（確認 recv 可被中斷）
3. `CancelIoEx` hook 增加「每次呼叫都記錄」（含非 shim handle），確認是否真的被呼叫
4. 若仍卡：用 `winedbg` / `gdb -p` 抓執行緒堆疊定位等待點

### 目前裝置狀態
- `Bus 008 Device 015`
- 最後一次 Stop 未送出 `02 04 00`，裝置可能又處於卡死狀態
- 需要 VM 重新設定 FPGA（或重新插拔）才能再測

---

## 2026-10-30 第三階段：Stop 已解（v4 + CancelIoEx hook）

### 修正內容（v4）
1. **只有 `02 04 00`（FIFO reset）會 abort pending IN**；
   `01 00 00`、`02 08 00` 改用於擷取中的 flow-pause／重新設定，不再 abort
   （v3 會把正常串流殺掉）。
2. **`CancelIoEx` IAT hook**（`install: dioc=1 cancelio=1 ciocp=1`）：
   命中 in-flight OVERLAPPED 時立即以 `STATUS_CANCELLED` post completion，
   並送 `REQ_ABORT_PIPE` 讓 helper 解除 USB 傳輸。

### 2026-10-30 16:39 整合測試（裝置先經 Windows VM 設定 FPGA 後交還 Linux）
序列：Run → 15 s 處改擷取設定 → Stop → Run → Stop → Run → Stop（3 個循環）

證據（helper / shim 日誌）：

| 項目 | 結果 |
|---|---|
| `cancelio.log` `matched pending, cancelled` | **8 次**（每次 Stop 都命中） |
| helper `abort ep=86` + 以 `-10` 結束 | 6 次；其中一笔以 3,104,768/5,005,312 partial + `-10` 交還（語義正確） |
| App 送 `02 04 00` | 3 次（每個 Stop 一次） |
| App 送 `02 08 00` | **301 次，均未觸發 abort**（v4 語義正確） |
| 完整 chunk（status=0，5,005,312 bytes，timeouts=0） | **254 個**（≈64 s 串流） |
| `-EPROTO` | **0** |
| 停止時的 `0x84` 1-byte 輪詢 | 出現 ×3（與 Windows 序列一致） |
| UI | 無凍結、無錯誤對話框；三次 Run 波形正常 |

**結論：Wine 下 Run→Stop→Run 穩定。**第二階段的 Stop 死結由
「CancelIoEx 未挂鉤 ＋ abort 條件過寬」造成，兩者已修復並驗證。

### 下一步（FPGA）
- 序列差異已分析完成 → `docs/analysis/fpga-upload-sequence.md`：
  Windows 成功流程在上傳後有 **EP2 zero-length IN** 並以 `b2` 讀取
  （`03 00` → `83 00`）判定完成；我們的 `dswfpga` 缺這兩步、
  且結尾多送 `b1 f0 02 00`。
- 依該文件第 4 節規格修 `dswfpga`（v2），在冷裝置上原生重試。

---

## 2026-10-30 第四階段：FPGA 自行設定打通（不再需要 Windows）

### 根因（見 `docs/analysis/fpga-upload-sequence.md` 的修正節）
- `b0 fc e8 1d`（PROG）之前裝置必須處於 idle：`b2 == 03 00`。
  v1/v2 在 `01 00`（忙碌）狀態下 PROG → 上傳完成但 FPGA 永不設定。
- pcap 中 `0x82` 的 zero-length IN **不會出現在 bus 上**（裝置沒有 EP 0x82），
  與失敗無關（假設 1 證偽）。

### dswfpga v3（`tools/`）
- 指令間 15 ms + PROG 前輪詢 `03 00` + 整段序列最多 3 次重試、去掉結尾 `f0`。
- 冷裝置（剛拔插）**第一次嘗試**即 `b2 → 83 00` CONFIGURED，
  不需要 USB reset、不需要 ZLP、不需要 VM。

### 移植到 helper（`DSW_FPGA_RECOVER` 改為預設開）
- `fpga_prepare()` 拆兩段：前置指令（15 ms 間隔）→ 等待 `03 00` →
  `fc e8 1d` / `fd 07 00`，App 的 466,408-byte OUT 照常接在後面。

### 2026-10-30 16:57 端到端驗證（冷裝置、純 Linux + Wine）
- 拔插（冷）→ 起 helper（`DSW_FPGA_RECOVER=1`）→ 起 DSWave：
  `BSP: Start FPGA configure: 466408` → `FPGA configure done: 466408`（0.3 s）
  → `status: READY`，無 timeout。
- 第一次 Run：**63 個完整 chunk（5,005,312 bytes、status=0、timeouts=0）**，
  `-EPROTO` 0 次；Run/Stop 反覆操作正常。
- `DSW_FPGA_RECOVER` 預設值改為 `1`（`0` 可關閉）。

**狀態總結：列舉 → FPGA 設定 → 擷取 → Stop → 再擷取，全程不需 Windows。**
