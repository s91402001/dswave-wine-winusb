# DSWave on Wine + WinUSB 卡點分析與解決建議 (`argon_advise.md`)

最後更新：2026-10-29

---

## 一、卡點根本原因定位（為什麼沒有呼叫 `CreateFile` 與 `WinUsb_Initialize`？）

對照 [progress.md](../progress.md) 與 [setupapi.log]($WINEPREFIX/drive_c/ProgramData/DSWaveShim/setupapi.log#L256-L273) 的呼叫序列：

```text
GetClassDevsA(NULL, NULL, flags=0x16)
EnumDeviceInterfaces idx=0 guid=f18a0e88   (GUID_DEVINTERFACE_USB_HUB)
EnumDeviceInterfaces idx=0 guid=a5dcbf10   (GUID_DEVINTERFACE_USB_DEVICE)
GetDetailA size=0 -> 198
GetInstanceIdA
GetRegPropA prop=34 / 4 / 17 / 18          (INSTALL_STATE / SERVICE / UPPERFILTERS / LOWERFILTERS)
EnumDeviceInterfaces idx=0 guid=3abf6f2d   (GUID_DEVINTERFACE_USB_HOST_CONTROLLER)
GetClassDevsA(NULL, "USB", flags=0x6)
GetInstanceIdA
GetRegPropA prop=9                         (SPDRP_DRIVER)
OpenDevRegKey key_type=1                   (DIREG_DEV)
EnumDeviceInterfaces idx=0 guid=4d1e55b2   (GUID_DEVINTERFACE_HID)
```

這個呼叫順序是**上游標準 `libusb-1.0` Windows 後端（`libusb/os/windows_winusb.c` 的 `winusb_get_device_list()`）**的多階段掃描流程（依序為 `HUB_PASS` → `DEV_PASS` → `HCD_PASS` → `GEN_PASS` → `HID_PASS` → `EXT_PASS`）。

**之所以掃描結束後完全沒有呼叫 `CreateFile` 或 `WinUsb_Initialize`，不是因為應用層安全檢查或 Profile 過濾，而是裝置在 `libusb_get_device_list()` 內部就被丟棄了（回傳 0 個裝置）。** 具體有 4 個環環相扣的斷點：

### 斷點 1（最直接死因）：`GEN_PASS` 檢查父節點（Parent Hub）失敗（`unlisted ancestor`）
1. 在 `DEV_PASS`（`a5dcbf10`）階段，`libusb` 只是為 `DevInst = 0x1000` 配置一個未初始化的 `libusb_device` 結構，記錄裝置路徑與 `WinUSB` API backend，**此時並不會開啟裝置，也不會讀取任何 Descriptor**。
2. 真正初始化裝置（`init_device`）是在 `GEN_PASS`（`GetClassDevsA(NULL, "USB", flags=0x6)`）執行完 `OpenDevRegKey` 之後：`libusb` 會呼叫 `get_ancestor()`，透過 **`Cfgmgr32.dll` 的 `CM_Get_Parent`** 往上尋找父節點（USB Hub），並在 `HUB_PASS` / `HCD_PASS` 已建立的裝置列表中查找該父 Hub 的 `libusb_device`。
3. 目前 [setupapi_shim.c](../../wine-winusb-shim/src/setupapi_shim.c#L833-L842) 有兩個致命問題：
   - **載入模組不同**：`libusb` 的 `CM_Get_Parent` 與 `CM_Get_Child` 是從 **`Cfgmgr32.dll`** 動態載入（`GetProcAddress`），而非 `SetupAPI.dll`，所以寫在 `setupapi_shim.c` 的 `CM_Get_Parent` 根本沒被呼叫到。
   - **沒有父 Hub 節點**：即使被呼叫到，`setupapi_shim.c` 對 `FAKE_DEVINST` 回傳 `CR_NO_SUCH_DEVNODE`，且 `HUB_PASS`（`f18a0e88`）與 `HCD_PASS`（`3abf6f2d`）在 `setupapi_shim.c` 都回傳 `FALSE`（0 個 Hub）。
   - 當找不到父 Hub 時，`libusb` 直接印出：
     `unlisted ancestor for 'USB\VID_2A0E&PID_0080\8&6' (non USB HID, newly connected, etc.) - ignoring`
     隨即呼叫 `libusb_unref_device()` **將剛建立的裝置銷毀**！

### 斷點 2：`libusb` 在列舉階段是對「父 Hub」發送 `DeviceIoControl` 讀 Descriptor，而非呼叫 `WinUsb_*`
- 在 Windows 架構下，`libusb_get_device_list()` 的 `init_device()` **完全不會開啟裝置本身，也不會呼叫 `WinUsb_Initialize` 或 `WinUsb_GetDescriptor`**。
- 相反地，它會 `CreateFileA` 開啟**父 Hub 的路徑**，並對父 Hub 發送 `DeviceIoControl`：
  - `IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX` (`0x220448`)：取得 18 bytes 的 `DeviceDescriptor`、`CurrentConfigurationValue`（必須非 0，否則視為 unconfigured 忽略）、`Speed`、`DeviceAddress`、`ConnectionStatus`（必須為 `1` `DeviceConnected`）。
  - `IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION` (`0x220410`)：快取所有 Configuration Descriptors。
- 只有當應用層後續明確呼叫 `libusb_open()` 時，`libusb` 才會去 `CreateFileA` 裝置路徑並呼叫 `WinUsb_Initialize()`。

### 斷點 3：`SetupDiOpenDevRegKey(DIREG_DEV)` 缺少 `DeviceInterfaceGUIDs`
- 在 `GEN_PASS` 中，`libusb` 呼叫 `SetupDiOpenDevRegKey(..., DIREG_DEV)` 後，會呼叫 `RegQueryValueExA` 讀取 `"DeviceInterfaceGUIDs"`（`REG_MULTI_SZ`）或 `"DeviceInterfaceGUID"`（`REG_SZ`）。
- 目前 [setupapi_shim.c](../../wine-winusb-shim/src/setupapi_shim.c#L806-L828) 在 `key_type == 1` (`DIREG_DEV`) 時未寫入 `DeviceInterfaceGUIDs`，導致 `libusb` 無法在 `EXT_PASS` 將介面路徑關聯到 `usb_interface[0]`。

### 斷點 4：尚未開啟 `libusb` 內建除錯日誌（`LIBUSB_DEBUG=4`）
- `DSWave.exe` 靜態連結的 `libusb` 保留了完整的除錯字串與環境變數檢查（`getenv("LIBUSB_DEBUG")`），開啟後會直接將 `libusb` 內部日誌輸出到 `stderr`。
- 只要在執行時加上 `LIBUSB_DEBUG=4`，就能直接在終端機看到 `libusb` 在哪一行放棄該裝置：
  ```bash
  LIBUSB_DEBUG=4 WINEPREFIX=~/.wine-dswave wine "C:\\Program Files\\DSWave\\DSWave.exe" -l 5
  ```

---

## 二、預先排雷：當 `libusb_get_device_list()` 通過後會遇到的下一個關卡

為了避免修好列舉後馬上又卡住，這裡一併列出通過 `libusb_get_device_list()` 後會立刻觸發的兩個問題：

1. **String Descriptor 的 `langid` 被 `helper.c` 忽略（會導致廠商/產品名稱驗證失敗）**
   - `DSWave.exe` 找到 `2a0e:0080` 後，會立即呼叫 `libusb_open()` 並讀取 String Descriptor：
     - 驗證 `iManufacturer` 字串必須為 `"DreamSourceLab"`
     - 驗證 `iProduct` 字串必須為 `"USB-based DSL Instrument v3"`
   - 在 [winusb_shim.c](../../wine-winusb-shim/src/winusb_shim.c#L292-L325) 的 `WinUsb_GetDescriptor()` 中，`langid` 被放在 `req.c` 傳給 helper；但 [helper.c](../../wine-winusb-shim/src/helper.c#L323-L358) 的 `do_descriptor()` 卻呼叫了：
     ```c
     libusb_get_descriptor(handles[id], (uint8_t)req->a, (uint8_t)req->b, buf, (int)req->len);
     ```
     `libusb_get_descriptor` 會把 `wIndex`（即 `langid`）硬塞為 `0`！當讀取 `iManufacturer` / `iProduct`（`type == 0x03, index > 0`）時，應該改用 `libusb_control_transfer(handles[id], LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR, (uint16_t)((req->a << 8) | req->b), (uint16_t)req->c, buf, (uint16_t)req->len, 5000)`，把 `req->c` (`langid`) 正確傳入 `wIndex`。

2. **USB Speed 數值映射差異**
   - `DSWave.exe` 的 `FindProfile` 會以 `(VID, PID, speed)` 三元組查 Profile 表。
   - 在 `libusb` 中，`enum libusb_speed` 的值為：`LOW = 1`, `FULL = 2`, `HIGH = 3`, `SUPER = 4`。
   - 若走 Hub IOCTL（`USB_NODE_CONNECTION_INFORMATION_EX.Speed`），Windows 定義 `UsbLowSpeed = 0, UsbFullSpeed = 1, UsbHighSpeed = 2, UsbSuperSpeed = 3`（`libusb` 內部會 `+1` 轉成 `1..4`）。
   - 若走 `WinUsb_QueryDeviceInformation(DEVICE_SPEED)`（[winusb_shim.c](../../wine-winusb-shim/src/winusb_shim.c#L336-L346)），Windows WinUSB API 定義則是 `LowSpeed = 0x01, FullSpeed = 0x02, HighSpeed = 0x03`。目前 [helper.c](../../wine-winusb-shim/src/helper.c#L93-L103) 的 `speed_to_windows()` 對 `HIGH` 回傳 `2`、對 `SUPER` 回傳 `3`，需留意兩處規範差異。

---

## 三、解決方案建議（依推薦程度排序）

面對上述架構限制，我們有三條路線可選。**強烈建議先考慮路線 A 或路線 B，最後才選路線 C**。

### 路線 A（最推薦、最乾淨）：在墊片 DLL 內直接攔截/轉發 `DSWave.exe` 內的 `libusb` 後端

- **核心思路**：
  `DSWave.exe` 本身其實是一個標準的 `libusb-1.0` 應用程式，它之所以會呼叫 `SetupAPI`、`Cfgmgr32`、`WinUSB` 與 `DeviceIoControl`，完全是因為它靜態連結了 `libusb` 的 Windows backend（`windows_winusb.c`）。
  與其在底層辛苦偽造整棵 Windows PnP 裝置樹（HCD + Root Hub + Hub + Device + Registry + IOCTL）去「哄騙」`windows_winusb.c`，再從 `winusb.dll` 把結果轉回 Linux `libusb`（等於 `libusb -> 假 Windows 樹 -> TCP -> Linux libusb`），不如直接在已載入的 `winusb.dll`（或 `setupapi.dll`）的 `DllMain(DLL_PROCESS_ATTACH)` 中，**直接替換或 Hook 主程式內的 `libusb` 函式（或 `usbi_os_backend` 結構體表）**，直接透過 TCP 與 `helper` 通訊！
- **優點**：
  - **徹底擺脫** `SetupAPI`、`Cfgmgr32`、`DeviceIoControl`、Hub 階層與 Registry 機碼細節。
  - `helper.c` 幾乎可以原封不動重用（因為 `helper.c` 本來就是 `libusb` 的薄包裝）。
  - 只需要對接 `libusb` 的少數幾個核心操作（列舉、開關裝置、讀 Descriptor、Claim Interface、Control/Bulk 傳輸）。

---

### 路線 B（善用 Wine 既有基礎）：檢查並結合 Wine 11.20 內建的 `wineusb.sys` PnP 樹

- **核心思路**：
  目前 `setupapi_shim.c` 在 `SetupDiGetClassDevsA` 遇到 `DIGCF_ALLCLASSES | DIGCF_DEVICEINTERFACE` (`flags=0x16`) 或 `"USB"` 時，直接用 `make_fake_set()` 取代了原本 `setupapi_orig.dll` 回傳的 `HDEVINFO`，**導致 Wine 自己建立的 Root Hub / HCD 裝置集合被墊片遮蔽了**！
- **驗證步驟**：
  1. 先開 `LIBUSB_DEBUG=4` 與 `WINEDEBUG=+wineusb,+plugplay,+mountmgr`，觀察在不攔截 `SetupAPI` 的情況下（或只攔截 `winusb.dll` 時），Wine 11.20 的 `wineusb.sys` 是否其實已經在 Registry (`HKLM\System\CurrentControlSet\Enum\USB\VID_2A0E&PID_0080\...`) 建立好完整的 HCD → Root Hub → USB 裝置樹與 Hub IOCTL？
  2. 如果 Wine 11.20 已經有完整的 `USB` 枚舉樹與 Root Hub IOCTL，而只是缺少：
     - 裝置的 `SPDRP_SERVICE = "WinUSB"`
     - `GUID_DEVINTERFACE_USB_DEVICE` 介面連結
     - `winusb.dll` 的實作（原本為 stub）
     那麼我們只需要在 Wine 的 Registry 補上該裝置的 `Service="WinUSB"` 與介面資訊，並保留你的 `winusb_shim.c` + `helper.c`，**完全不需要 `setupapi_shim.c` 的 `make_fake_set()`**！這值得一試（只需 5 分鐘測試）。

---

### 路線 C（延續現有 `setupapi_shim.c` 合成路線）：補齊 HCD / Hub / Cfgmgr32 / Hub IOCTL

如果你想維持目前 `setupapi_shim.c` 完全自建假裝置的架構，則必須補齊 `libusb` `windows_winusb.c` 要求的完整拓樸與 IOCTL，共 4 個步驟：

1. **攔截 `Cfgmgr32.dll` 的 `CM_Get_Parent` 與 `CM_Get_Child`**
   - 建立 `cfgmgr32.dll` 墊片（或在 `kernel32!GetProcAddress` 攔截），定義三個假 `DEVINST`：
     - `FAKE_HCD_DEVINST (0x3000)` ──(`CM_Get_Child`)──> `FAKE_HUB_DEVINST (0x2000)`
     - `FAKE_DEV_DEVINST (0x1000)` ──(`CM_Get_Parent`)──> `FAKE_HUB_DEVINST (0x2000)`
     - `FAKE_HUB_DEVINST (0x2000)` ──(`CM_Get_Parent`)──> `FAKE_HCD_DEVINST (0x3000)`

2. **在 `setupapi_shim.c` 補上 `HCD_PASS` 與 `HUB_PASS` 的枚舉**
   - `GUID_DEVINTERFACE_USB_HOST_CONTROLLER` (`{3ABF6F2D-71C4-462A-8A92-1E6861E6AF27}`)：
     回傳 1 個 HCD（`DevInst = 0x3000`，Instance ID 為 `"PCI\\VEN_8086&DEV_1E31\\0"` 以通過 `PCI\VEN_%04hx&DEV_%04hx` 解析）。
   - `GUID_DEVINTERFACE_USB_HUB` (`{F18A0E88-C30C-11D0-8815-00A0C906BED8}`)：
     回傳 1 個 Root Hub（`DevInst = 0x2000`，Instance ID 為 `"USB\\ROOT_HUB30\\0"`，路徑指向一個真實空檔案如 `\\?\C:\ProgramData\DSWaveShim\ROOT_HUB`）。

3. **在 `SetupDiOpenDevRegKey(DIREG_DEV)` 補上 `DeviceInterfaceGUIDs`**
   - 在 `setupapi_shim.c` 的 `SetupDiOpenDevRegKey` (`key_type == 1`) 寫入：
     - `DeviceInterfaceGUIDs` (`REG_MULTI_SZ`)：`L"{DEE824EF-729B-4A0E-9C14-B7117D33A817}\0\0"`
     - `SymbolicName` (`REG_SZ`)：對應的裝置路徑字串。

4. **攔截對 Root Hub 檔案 handle 的 `DeviceIoControl`**
   - 當 `libusb` 開啟 `ROOT_HUB` 檔案並呼叫 `DeviceIoControl` 時，回應以下 3 個 IOCTL：
     - `IOCTL_USB_GET_NODE_INFORMATION` (`0x220408`)：回傳 `USB_NODE_INFORMATION`，其中 `HubDescriptor.bNumberOfPorts = 16`。
     - `IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX` (`0x220448`)（與 `_V2` `0x22045c`）：當查詢 `ConnectionIndex == f->dev.addr` 時，填入從 `helper` 讀到的 18-byte `DeviceDescriptor`、`CurrentConfigurationValue = 1`、`Speed = 3`、`DeviceAddress = f->dev.addr`、`ConnectionStatus = 1 (DeviceConnected)`。
     - `IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION` (`0x220410`)：回傳 12-byte `USB_DESCRIPTOR_REQUEST` 標頭加上從 `helper` 讀到的 `ConfigurationDescriptor`。

---

## 四、建議的下一步行動清單

1. **先開 `LIBUSB_DEBUG=4` 驗證診斷結論（1 分鐘）**
   執行：
   ```bash
   LIBUSB_DEBUG=4 WINEPREFIX=~/.wine-dswave wine "C:\\Program Files\\DSWave\\DSWave.exe" -l 5
   ```
   確認終端機印出 `unlisted ancestor for 'USB\VID_2A0E&PID_0080\8&6' ... - ignoring`。
2. **測試路線 B（5 分鐘）**
   暫時移除 `setupapi=native`（只留 `winusb=native`），搭配 `LIBUSB_DEBUG=4` 看 Wine 11.20 原生的 `wineusb.sys` + `setupapi.dll` 能走到 `winusb_get_device_list()` 的哪一步。
3. **修復 `helper.c` 的 `do_descriptor()` `langid` 問題（5 分鐘）**
   將 `helper.c` 中的 `libusb_get_descriptor` 改為帶 `wIndex = (uint16_t)req->c` 的 `libusb_control_transfer`，確保後續 `"DreamSourceLab"` 與 `"USB-based DSL Instrument v3"` 字串檢查能一次通過。
4. **依路線 B 測試結果選擇實作路線 A（直接 Hook `libusb`）或路線 C（補齊 Hub 樹與 IOCTL）**。
