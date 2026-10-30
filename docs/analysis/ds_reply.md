# 對 `argon_advise.md` 的驗證與執行計畫 (`ds_reply.md`)

最後更新：2026-10-29
狀態：已驗證診斷、選定路線、等待執行

---

## 一、驗證方法

不是只看建議，而是直接對照三份證據：

1. **上游 libusb 原始碼**
   `https://raw.githubusercontent.com/libusb/libusb/master/libusb/os/windows_winusb.c`
2. **`DSWave.exe` 二進位內容**
   `strings`、`objdump -p`（匯入表、匯出表）
3. **既有實測紀錄**
   `/tmp/opencode/relay3.log`（`WINEDEBUG=+relay`）、
   `~/.wine-dswave/drive_c/ProgramData/DSWaveShim/setupapi.log`

---

## 二、逐條驗證結果

| # | argon 的說法 | 驗證方式 | 判定 |
|---|---|---|---|
| 1 | DSWave 使用上游 libusb 的 Windows backend | exe 內含 `unlisted ancestor for '%s' (non USB HID, newly connected, etc.) - ignoring`、`winusb_get_device_list`、`libusb_get_device_list` | ✅ 正確 |
| 2 | 掃描是多階段（HUB→DEV→HCD→GEN→HID→EXT） | 原始碼 `HUB_PASS=0 … EXT_PASS=5`（L2001-2006）；GUID 對應 L2039-2042 | ✅ 正確 |
| 3 | 卡在 `GEN_PASS` 找不到父 Hub | 原始碼 L2217-2231：`get_ancestor()` 失敗 → `unlisted ancestor` → `continue` | ✅ 正確 |
| 4 | `CM_Get_Parent` 來自 `Cfgmgr32.dll`，不是 setupapi | relay 顯示 `LoadLibraryA("C:\\windows\\system32\\Cfgmgr32.dll")`；setupapi 的 CM_* 呼叫為 0 | ✅ 正確（我們寫在 setupapi 的 CM_* 從未被呼叫） |
| 5 | 列舉階段不會呼叫 `WinUsb_*`，而是對父 Hub 發 IOCTL | 原始碼 `init_device()` L1386-1560：`CreateFileA(parent_priv->path)` + `IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX` + `cache_config_descriptors()` | ✅ 正確 |
| 6 | 需要 HCD 與 Hub 兩層假節點 | 原始碼 `HCD_PASS`→`GUID_DEVINTERFACE_USB_HOST_CONTROLLER`、`HUB_PASS`→`GUID_DEVINTERFACE_USB_HUB`、`enumerate_hcd_root_hub()` 用 `CM_Get_Child` | ✅ 正確 |
| 7 | `SetupDiOpenDevRegKey(DIREG_DEV)` 缺 `DeviceInterfaceGUIDs` 會擋住 | 原始碼：該值只在 `EXT_PASS` 被 `get_guid()` 讀取，用來附加「額外介面路徑」；非複合裝置的 `usb_interface[0].path` 已在 `DEV_PASS` 設好 | ⚠️ **非必要**（可選） |
| 8 | 建議路線 A：hook 主程式內的 libusb | libusb 是**靜態連結**在 `DSWave.exe` 內，函式呼叫是直接位址，不在 IAT、也不在匯出表（exe 的 30 個匯出是 `fftw_*`/`HAL_Log*` 等，不是 libusb） | ❌ **不建議**（需 inline hook / 改 `.text`，極脆弱） |
| 9 | helper 的 `do_descriptor()` 忽略 langid | `libusb_get_descriptor()` 會把 `wIndex` 固定為 0；讀字串描述元必須用 `libusb_control_transfer(..., wIndex=langid, ...)` | ✅ 正確，必修 |
| 10 | 速度值語意有差異 | Hub IOCTL 的 `Speed` 是 Windows 值（`UsbSuperSpeed=3`）；`WinUsb_QueryDeviceInformation(DEVICE_SPEED)` 是另一套值 | ✅ 需校正（M2 時用 log 對照） |

### 額外查到的關鍵細節

- 我們的 `setupapi.log` 序列（`GetClassDevsA(NULL,NULL,0x16)` →
  `EnumDeviceInterfaces` f18a0e88 / a5dcbf10 / 3abf6f2d →
  `GetClassDevsA(NULL,"USB",0x6)` → `OpenDevRegKey` → HID）**完全對應 libusb 的 pass 順序**，
  證實不是應用層過濾。
- `get_dev_port_number()`（L1600-1635）會先讀 `SPDRP_LOCATION_INFORMATION`，
  格式 `"Port_#0006.Hub_#0008"`；我們目前的回傳剛好可用。
- Root Hub 的 `init_root_hub()`（L1210）只要 instance ID 含 `ROOT_HUB30`
  就直接判定 SuperSpeed，**不需要** V2 IOCTL、也不需要逐 port 查詢。
- `get_api_type()` 用 `_stricmp` 比對 `SPDRP_SERVICE`，所以 `"WinUSB"` 可匹配 `winusb`。
- DSWave 靜態匯入 `DeviceIoControl`、`CreateFileA/W`（KERNEL32），
  所以 **IAT hook 可行**。

---

## 三、選定路線

**採用路線 C（補齊 libusb 需要的假 PnP 樹），但縮小到最小集合。**
不採用路線 A（hook libusb），理由見上表 #8。

### 需要的元件

```
DSWave.exe
 ├─ SetupAPI (shim)      → 三顆假裝置：HCD / HUB / DEV
 ├─ Cfgmgr32 (shim)      → CM_Get_Parent / CM_Get_Child 的父子關係
 ├─ DeviceIoControl(IAT) → 只攔假 Hub handle 的 3 個 IOCTL
 └─ WinUSB (shim, 已有)  → 真正開裝置 + bulk/control（usbscan 已驗證）
        └─ TCP → helper (libusb) → /dev/bus/usb/008/006
```

### 3.1 `cfgmgr32.dll` 墊片（新增）

| 函式 | 行為 |
|---|---|
| `CM_Get_Parent(0x1000)` | 回 `0x2000`（HUB），`CR_SUCCESS` |
| `CM_Get_Parent(0x2000)` | `CR_NO_SUCH_DEVNODE`（root hub 沒有父） |
| `CM_Get_Parent(0x3000)` | `CR_NO_SUCH_DEVNODE` |
| `CM_Get_Child(0x3000)` | 回 `0x2000`（`enumerate_hcd_root_hub` 需要） |
| `CM_Get_Child(0x2000)` | 回 `0x1000`（備用） |
| 其他 | forward 回 `cfgmgr32_orig.dll` |

### 3.2 `setupapi.dll` 墊片：三裝置表

| DevInst | Instance ID | 介面 GUID | 必要屬性 |
|---|---|---|---|
| `0x3000` HCD | `PCI\VEN_8086&DEV_1E31&SUBSYS_00000000&REV_00\0` | `{3ABF6F2D-71C4-462A-8A92-1E6861E6AF27}` | 能被 `PCI\VEN_%04hx&DEV_%04hx` 解析 |
| `0x2000` HUB | `USB\ROOT_HUB30\4&0&0` | `{F18A0E88-C30C-11D0-8815-00A0C906BED8}` | `SPDRP_ENUMERATOR_NAME="USB"`；**無** port / **無** parent；路徑指向真實檔案 `ROOT_HUB` |
| `0x1000` DEV | `USB\VID_2A0E&PID_0080\8&6` | `{A5DCBF10-6530-11D2-901F-00C04FB951ED}`（可再加 `{DEE824EF-…}`） | `LOCATION_INFORMATION="Port_#0006.Hub_#0008"`；`SERVICE="WinUSB"`；`INSTALL_STATE=0` |

重點：
- `SetupDiGetClassDevsA(NULL,NULL,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE)`（flags `0x16`）
  要回三顆裝置（libusb 的介面集合）。
- `SetupDiGetClassDevsA(NULL,"USB",DIGCF_PRESENT|DIGCF_ALLCLASSES)`（flags `0x6`）
  要回 DEV（`GEN_PASS` 的 enumerator 集合），**DevInst 必須同為 `0x1000`**。
- `SetupDiEnumDeviceInterfaces(set, devinfo, guid, 0, ...)` 要依「這個 devinfo 是哪顆裝置 + 要求的 GUID」
  决定回傳或 `ERROR_NO_MORE_ITEMS`。

### 3.3 IAT hook `DeviceIoControl`

在 `winusb.dll`（或獨立 shim）的 `DllMain` 中，找到 `DSWave.exe` 的
KERNEL32 import thunk，把 `DeviceIoControl` 換成我們的函式：

```c
BOOL WINAPI hook_DeviceIoControl(HANDLE h, DWORD code, LPVOID in, DWORD in_len,
                                 LPVOID out, DWORD out_len, LPDWORD ret, LPOVERLAPPED ov);
```

判斷 `h` 是否為假 Hub 的 handle（`GetFinalPathNameByHandleW` 比對 `ROOT_HUB`）：

| IOCTL | 代碼 | 回應 |
|---|---|---|
| `IOCTL_USB_GET_NODE_INFORMATION` | `0x220408` | `USB_NODE_INFORMATION`，`HubDescriptor.bNumberOfPorts=8` |
| `IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX` | `0x220448` | 18-byte device descriptor（helper）、`CurrentConfigurationValue=1`、`Speed=3`、`DeviceAddress=6`、`ConnectionStatus=1` |
| `IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX_V2` | `0x22045c` | 可回失敗（非致命），或填 SuperSpeed flags |
| `IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION` | `0x220410` | `USB_DESCRIPTOR_REQUEST` 標頭 + config descriptor（helper） |
| 其他 handle / code | — | 轉回原始 `DeviceIoControl` |

注意：
- 一律依 libusb 傳入的 `*ret`/`out_len` 填寫，不越界。
- 有 `LPOVERLAPPED` 時同步完成並 `SetEvent(ov->hEvent)`，回 `TRUE`。

### 3.4 選用：Enum key 補 `DeviceInterfaceGUIDs`

`DeviceInterfaceGUIDs`（`REG_MULTI_SZ`）= `{DEE824EF-729B-4A0E-9C14-B7117D33A817}`。
只影響 `EXT_PASS`，不是列出裝置的必要條件；便宜，順手做。

---

## 四、順手必修的兩個 bug

1. **`helper.c` `do_descriptor()` 的 langid**
   改為：
   ```c
   libusb_control_transfer(handles[id], LIBUSB_ENDPOINT_IN,
       LIBUSB_REQUEST_GET_DESCRIPTOR,
       (uint16_t)((req->a << 8) | req->b),   /* wValue */
       (uint16_t)req->c,                      /* wIndex = langid */
       buf, (uint16_t)req->len, 5000);
   ```
   DSWave 會驗證 `iManufacturer="DreamSourceLab"`、`iProduct="USB-based DSL Instrument v3"`。

2. **速度映射**
   - Hub IOCTL `Speed` 用 Windows 值：`UsbSuperSpeed = 3`
   - `WinUsb_QueryDeviceInformation(DEVICE_SPEED)` 依 WinUSB 規範（1..4 的對應）
   - M2 打通後用 `LIBUSB_DEBUG` 與 DSWave log 對照校正

---

## 五、執行順序與驗收

| 階段 | 工作 | 驗收條件 | 估時 |
|---|---|---|---|
| **0** | `LIBUSB_DEBUG=4` 跑 DSWave，確認 `unlisted ancestor`；relay 確認 IOCTL code | 看到 libusb 自己的除錯輸出，且與原始碼行號對上 | 0.5 h |
| **1** | `cfgmgr32` 墊片 + 三裝置表 | `LIBUSB_DEBUG` 出現 `(bus:1, addr:6, depth:2)`，`libusb_get_device_list` 回 1 | 0.5–1 d |
| **2** | IAT hook + 4 個 IOCTL（descriptor/config 由 helper 供） | DSWave 呼叫 `WinUsb_Initialize`，`winusb.log` 有 `opened 2a0e:0080` | 0.5–1 d |
| **3** | langid 修正、字串驗證、claim interface | DSWave 出現裝置 tab / 讀到型號 | 0.5 d |
| **4** | 低速率擷取 | 低取樣率可擷取、波形正確 | 0.5–1 d |

**總計 1.5–3 個工作天。**

---

## 六、風險與對策

| 風險 | 對策 |
|---|---|
| IAT hook 位置錯誤 | 用 `ImageDirectoryEntryToData` 掃 import descriptor，只改 `KERNEL32!DeviceIoControl` 一項，失敗則 fallback 不 hook |
| `USB_NODE_CONNECTION_INFORMATION_EX` 是可變長結構 | 以 libusb 傳入的 `out_len` 為上限填寫 |
| 重疊 I/O 語意 | hook 內同步完成 + `SetEvent`，回 `TRUE` |
| libusb 版本差異 | 已用 master 對照，exe 字串完全吻合 |
| Wine 載入 builtin 而非我們的 DLL | 已解：`DllOverrides=native` + system32 覆蓋（install.sh） |
| 若 M2 失敗 | 用 `LIBUSB_DEBUG=4` 的下一行錯誤定位；最終備案為 VM `usb-host` 直通 |

---

## 七、對 `argon_advise.md` 的結論

- 診斷（斷點 1、2）**正確且已用上游原始碼證實**。
- 斷點 3（`DeviceInterfaceGUIDs`）**不是必要條件**，只影響 `EXT_PASS`。
- 路線 A（hook libusb）**不可行/不划算**：libusb 是靜態連結、直接呼叫。
- 路線 B（改用 Wine 原生樹）**不可行**：Wine 的 `wineusb.sys` 沒有 function driver、
  不暴露使用者模式介面，libusb 的 HUB/HCD pass 會拿到 0 顆。
- 建議採用 **路線 C 的最小集合**（本文件第三章）。

---

## 八、目前系統狀態（承接 `progress.md`）

- `~/.wine-dswave` 已安裝墊片：`setupapi=native`、`winusb=native`，
  system32 已備份為 `setupapi_wine.dll` / `winusb_wine.dll`，另有 `setupapi_orig.dll`
- helper 可執行檔：`wine-winusb-shim/build/helper`（執行中）
- `usbscan.exe` 已端到端通過（列舉→CreateFile→WinUsb_Initialize→3 endpoints）
- DSWave 掃描已打到墊片，但停在 `libusb_get_device_list()` 內部（本文件要修的部分）
- 裝置在 host、VM 關機中
