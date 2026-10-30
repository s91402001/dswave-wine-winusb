# 對 `ds_reply.md` 的審視與技術覆核 (`argon_reply.md`)

最後更新：2026-10-29

---

## 一、總體評價

[ds_reply.md](ds_reply.md) 的查證非常紮實，將問題收斂為 **「路線 C 最小集合（`setupapi` 三裝置表 + `cfgmgr32` 父子關係 + IAT Hook `DeviceIoControl`）」** 是目前兼顧開發成本與執行穩定度的最佳工程決策。

以下針對 [ds_reply.md](ds_reply.md) 的內容，逐項列出**完全同意的部分**（包含 `ds_reply.md` 比 `argon_advise.md` 更精準的修正），以及**不同意或必須修正的 5 個關鍵實作細節**（若按原計畫實作，會在階段 1 與階段 2 卡住）。

---

## 二、完全同意的觀點

1. **同意放棄路線 A（Hook `libusb`）與路線 B，改採「路線 C 最小集合 + IAT Hook `DeviceIoControl`」**
   - `libusb` 靜態連結於 `DSWave.exe` 的 `.text` 區段，內部呼叫皆為相對位址直接跳轉，做 inline hook 確實脆弱。
   - 相對地，`DSWave.exe` 內的 `winusb_get_device_list()` 呼叫 `CreateFileA`（IAT `0x14109291c`）與 `DeviceIoControl`（IAT `0x14109294c`）皆透過 `.idata` 匯入表間接呼叫。在 `winusb.dll` 或 `setupapi.dll` 的 `DllMain` 中以 `VirtualProtect` 修改 IAT 指標來攔截 `DeviceIoControl`，乾淨且穩定。

2. **同意 #7：`SetupDiOpenDevRegKey(DIREG_DEV)` 的 `DeviceInterfaceGUIDs` 為「非必要（可選）」**
   - `ds_reply.md` 這點查證得比 `argon_advise.md` 更精準。
   - 核對 `DSWave.exe` 在 `DEV_PASS` 的反組譯（`0x14033c623`–`0x14033c67c`）：當 `api == USB_API_WINUSBX (3)` 時，在 `DEV_PASS` 當下就會直接執行 `priv->usb_interface[0].path = _strdup(dev_interface_path)`，並把 `priv->usb_interface[0..31].apib` 設為 `WinUSBX` 後端。因此單一介面（non-composite）裝置確實不需要靠 `GEN_PASS` 的 `DeviceInterfaceGUIDs` 與 `EXT_PASS` 來綁定介面路徑。

3. **同意 `ROOT_HUB30` 的簡化設計**
   - 核對 `DSWave.exe` 的 `init_root_hub()`（`0x14033d5bc`）：只要 Root Hub 的 Instance ID 含有 `"ROOT_HUB30"` 字串，呼叫完 `IOCTL_USB_GET_NODE_INFORMATION` (`0x220408`) 後就會直接 `CloseHandle` 並填寫 USB 3.0 Root Hub 預設描述元（`0x1d6b:0x0003`），不需要實作逐 port 的連線查詢或 V2 IOCTL。

4. **同意 [helper.c](../../wine-winusb-shim/src/helper.c) `do_descriptor()` 改用 `libusb_control_transfer` 帶入 `langid`，以及沿用 `SPDRP_LOCATION_INFORMATION` (`Port_#0006.Hub_#0008`)**。

---

## 三、不同意與必須修正的部分（5 個會導致實作卡住的陷阱）

### 1. 【不同意 / 需修正】第 5 節：階段 1 與階段 2 的驗收條件倒置

- **`ds_reply.md` 原文（第 5 節）**：
  > - 階段 1：`cfgmgr32` 墊片 + 三裝置表 → 驗收條件：`LIBUSB_DEBUG` 出現 `(bus:1, addr:6, depth:2)`，`libusb_get_device_list` 回 1
  > - 階段 2：IAT hook + 4 個 IOCTL → 驗收條件：DSWave 呼叫 `WinUsb_Initialize`
- **為什麼不對**：
  在 `libusb` 的 `init_device()` 中，印出 `(bus: %u, addr: %u, depth: %u, port: %u)`（`0x14033c74d`）以及將裝置塞入回傳清單的 `discovered_devs_append()`（`0x14033c7a4`），是在**成功執行完父 Hub 的 `CreateFileA` 與所有 `DeviceIoControl`（`0x220448` 及兩次 `0x220410`）之後**才會到達的最後一步！
  如果在階段 1 還沒有掛上 `DeviceIoControl` 的 IAT Hook，對真實空檔案 `ROOT_HUB` 呼叫 `DeviceIoControl(0x220448)` 必然回傳失敗，`init_device()` 會在 `0x14033bc37` 跳轉印出 `could not get node connection information for device` 並將裝置丟棄，**此時不會印出 `(bus:1, addr:6, ...)`，`libusb_get_device_list` 也依然會回傳 `0`**。
- **修正建議**：
  - **階段 1 的正確驗收條件**：`LIBUSB_DEBUG=4` 不再出現 `unlisted ancestor`，且能看到：
    1. `assigning Root Hub 'USB\ROOT_HUB30\4&0&0' bus number 1`
    2. `could not get node connection information for device 'USB\VID_2A0E&PID_0080\8&6'`（證明已成功通過 `CM_Get_Parent` 與父 Hub 綁定，推進到 Hub IOCTL 門口）。
  - **階段 2 的正確驗收條件**：補上 IAT Hook + Hub IOCTL 後，`LIBUSB_DEBUG=4` 才會印出 `(bus: 1, addr: 6, depth: 1, port: 6)`（註：直接掛在 Root Hub 下的裝置 `depth` 為 `0 + 1 = 1` 而非 `2`），且 `DSWave` 隨即呼叫 `WinUsb_Initialize`。

---

### 2. 【不同意 / 需修正】第 3.2 節：`SetupDiGetClassDevsA(NULL, "USB", flags=0x6)` (`GEN_PASS`) 應同時回傳 `HUB (0x2000)` 與 `DEV (0x1000)`，而非只回傳 `DEV`

- **`ds_reply.md` 原文（第 3.2 節）**：
  > `SetupDiGetClassDevsA(NULL,"USB",DIGCF_PRESENT|DIGCF_ALLCLASSES)`（flags `0x6`）要回 DEV（`GEN_PASS` 的 enumerator 集合），**DevInst 必須同為 `0x1000`**。
- **為什麼有問題**：
  在 `windows_winusb.c` 中，`HUB_PASS` (`pass == 0`) 掃到 Root Hub (`0x2000`) 時，只做了 `priv->root_hub = true` 與 `dev->bus_number = 1`，**並沒有呼叫 `init_root_hub()`**！
  真正的 `init_root_hub()`（包含 `ds_reply.md` 第 3.3 節列出的 `IOCTL_USB_GET_NODE_INFORMATION = 0x220408` 與 `"ROOT_HUB30"` 判斷）是在 **`GEN_PASS`（`flags = 0x6`，列舉 `"USB"`）掃到 `0x2000 (HUB)` 時才會被觸發**（見 `0x14033c9ea: cmpb $0x0, 0x51(%rbx)` → `0x14033bb5e: je init_root_hub`）。
  如果 `flags = 0x6` 只回傳 `DEV (0x1000)` 而不回傳 `HUB (0x2000)`，那 `init_root_hub()` 與 `0x220408` IOCTL 永遠不會被呼叫，Root Hub 的 `priv->initialized` 會停留在 `0`。
- **修正建議**：
  - `SetupDiGetClassDevsA(NULL, "USB", flags=0x6)` 應回傳 **`0x2000 (HUB)` 與 `0x1000 (DEV)` 兩顆裝置**（且建議 index 0 為 `0x2000`、index 1 為 `0x1000`）。

---

### 3. 【不同意 / 需修正】第 3.2 節：`SetupDiGetDeviceInterfaceDetailA/W` 的 `devinfo` 為 `NULL`，且現有 `setupapi_shim.c` 忽略了 `PSP_DEVINFO_DATA` 與 `PSP_DEVICE_INTERFACE_DATA`

- **`ds_reply.md` 原文（第 3.2 節）**：
  > `SetupDiEnumDeviceInterfaces(set, devinfo, guid, 0, ...)` 要依「這個 devinfo 是哪顆裝置 + 要求的 GUID」決定回傳或 `ERROR_NO_MORE_ITEMS`。
- **潛在陷阱（有兩處）**：
  1. **介面路徑錯亂陷阱**：當 `SetupDiEnumDeviceInterfaces` 回傳 `TRUE` 後，`libusb` 接著呼叫 `SetupDiGetDeviceInterfaceDetailA(set, &dev_interface_data, detail, size, &required, NULL)` 時（見 `0x14033b561` / `0x14033b5c6`），**第 6 個參數 `PSP_DEVINFO_DATA info` 傳的是 `NULL`**！
     而目前 [setupapi_shim.c](../../wine-winusb-shim/src/setupapi_shim.c#L413) 寫了 `(void)iface;` 完全忽略第 2 個參數 `iface`。當同一個 `HDEVINFO` set 內含三顆裝置時，`SetupDiGetDeviceInterfaceDetailA/W` 若不看 `iface`，就不知道現在要回傳 `HCD`、`ROOT_HUB` 還是 `2a0e:0080` 的檔案路徑！
     - **解法**：在 `SetupDiEnumDeviceInterfaces` 成功時，把該裝置的 `DevInst`（`0x3000` / `0x2000` / `0x1000`）存入 `iface->Reserved`，並在 `SetupDiGetDeviceInterfaceDetailA/W` 根據 `iface->Reserved`（或 `iface->InterfaceClassGuid`）回傳對應的檔案路徑。
  2. **屬性混用陷阱**：目前 [setupapi_shim.c](../../wine-winusb-shim/src/setupapi_shim.c#L518) 的 `SetupDiGetDeviceInstanceId*` 與 `SetupDiGetDeviceRegistryProperty*` 都寫了 `(void)info;`。改為三裝置表時，**必須嚴格根據 `info->DevInst` 分派屬性**。特別注意：當 `info->DevInst == 0x2000 (HUB)` 時，查詢 `SPDRP_ADDRESS` 或 `SPDRP_LOCATION_INFORMATION` **必須回傳失敗（`ERROR_INVALID_DATA`）**；若不小心回傳了 `DEV` 的 `Port_#0006`，`HUB_PASS` 在 `0x14033da42` 會因為 `port_nr != 0` 而認為它不是 Root Hub，導致 `bus_number` 保持為 `0`！

---

### 4. 【重要修正】第 3.3 節：`DeviceIoControl` 結構體的 `#pragma pack(1)` 對齊與 `0x220410` 的兩階段 `*ret` 長度檢查

- **陷阱 A：`USB_NODE_CONNECTION_INFORMATION_EX` (35 bytes) 必須 `#pragma pack(1)`**
  - 在 `DSWave.exe` 的 `init_device()`（`0x14033bc0c`–`0x14033bf08`）中，緩衝區大小固定為 `0x23` (35 bytes)，且直接從奇數位址讀取欄位：
    - `+0x00` (4B): `ConnectionIndex`（輸入為 `port_nr = 6`，必須檢查若 `ConnectionIndex != 6` 則設 `ConnectionStatus = 0 (NoDeviceConnected)`）
    - `+0x04` (18B): `DeviceDescriptor`（`+0x04` 必為 `0x12`，`+0x05` 必為 `0x01`）
    - `+0x16` (1B): `CurrentConfigurationValue`（必為 `1`）
    - `+0x17` (1B): `Speed`（`3` = `UsbSuperSpeed`）
    - `+0x18` (1B): `DeviceIsHub`（`0`）
    - `+0x19` (2B, **未對齊**): `DeviceAddress`（`6`）
    - `+0x1B` (4B, **未對齊**): `NumberOfOpenPipes`（`3`）
    - `+0x1F` (4B, **未對齊**): `ConnectionStatus`（`1` = `DeviceConnected`）
  - 若在 C 中定義該 struct 時沒有加上 `#pragma pack(push, 1)` 或 `__attribute__((packed))`，編譯器會在 `+0x19` 前插入 padding，導致 `DeviceAddress` 與 `ConnectionStatus` 位移錯亂而失敗。

- **陷阱 B：`IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION` (`0x220410`) 的第一次呼叫硬性檢查 `*ret == 21 (0x15)`**
  - `cache_config_descriptors()` 會對 `0x220410` 連續呼叫兩次：
    1. **第一次（dummy 探測，`0x14033bd8f`）**：`out_len = 21` (`0x15`，即 12-byte `USB_DESCRIPTOR_REQUEST` 標頭 + 前 9-byte `USB_CONFIGURATION_DESCRIPTOR`)。在 `0x14033bda2` 有一條硬性檢查：
       `cmpl $0x15, 0xf0(%rbp)`（**`*ret` 必須剛好等於 `21`**，否則直接報 `unexpected configuration descriptor 0 size (dummy)` 並放棄初始化）。
    2. **第二次（actual 讀取，`0x14033be43`）**：`out_len = 12 + wTotalLength`。在 `0x14033be59` 硬性檢查 **`*ret == 12 + wTotalLength`**，且 `out[13] == 0x02` (`bDescriptorType == USB_CONFIGURATION_DESCRIPTOR_TYPE`)。
  - 因此在處理 `0x220410` 時，除了保留輸入的前 12 bytes 標頭外，複製 descriptor 資料的長度必須是 `copy_len = min(out_len - 12, full_config_len)`，且務必設定 `*ret = 12 + copy_len`。

---

### 5. 【重要修正】第 3.3 節：Hub IOCTL 向 `helper` 索取 Descriptor 時，不宜直接呼叫現有的 `REQ_DESCRIPTOR`

- **`ds_reply.md` 原文（第 3.3 / 5 節）**：
  > IAT hook + 4 個 IOCTL（descriptor/config 由 helper 供）
- **為什麼要注意**：
  目前 [helper.c](../../wine-winusb-shim/src/helper.c#L323-L358) 的 `REQ_DESCRIPTOR` (`do_descriptor`) **必須傳入有效的 `req->handle`**（也就是必須先發 `REQ_OPEN`）。
  然而：
  1. 在 `winusb_get_device_list()` 發送 Hub IOCTL 的時間點，`WinUsb_Initialize` 還沒有被呼叫，因此 `winusb.dll` 手上還沒有任何已開啟的 `req->handle`。
  2. `helper.c` 的 `REQ_OPEN` 會執行 `libusb_open()` + **`libusb_claim_interface(h, 0)`**。如果為了回答 Hub IOCTL 而去呼叫 `REQ_OPEN`，若沒立刻 `REQ_CLOSE` 會佔住 interface 0 並耗盡 `MAX_HANDLES (8)`；就算每次 IOCTL 都 `REQ_OPEN` + `REQ_CLOSE`，也會在每次掃描時無謂地反覆 claim/release 實體介面。
- **修正建議**：
  - 在 Linux 原生 `libusb` 中，`libusb_get_device_descriptor(dev, &desc)` 與 `libusb_get_active_config_descriptor(dev, &conf)` 是直接讀取 Linux kernel 快取在 sysfs 的描述元，**完全不需要 `libusb_open()` 或 `claim_interface()`**。
  - 建議直接在 `helper.c` 的 `REQ_LIST` 回傳結構 `struct dev_info` 中，**順便帶回 18-byte `device_descriptor` 與 `config_descriptor`（或新增一個不需 handle 的 `REQ_GET_RAW_DESC`）**。這樣 `setupapi.dll` / IOCTL Hook 在列舉階段完全不必打開真實 USB 裝置就能秒回所有 Hub IOCTL，直到 DSWave 真正呼叫 `WinUsb_Initialize` 時才執行一次 `REQ_OPEN`。

---

## 四、修正後的建議執行順序總結

| 步驟 | 修改檔案 | 關鍵檢核點 |
|---|---|---|
| **Step 1** | `helper.c` / `proto.h` | 1. 修正 `do_descriptor()` 改用 `libusb_control_transfer` 傳遞 `wIndex = req->c` (`langid`)。<br>2. 在 `REQ_LIST`（或新指令）直接透過 `libusb_get_device_descriptor` / `libusb_get_active_config_descriptor` 回傳 18-byte Device Descriptor 與 Config Descriptor，不需 `REQ_OPEN`。 |
| **Step 2** | `cfgmgr32_shim.c`（新增）+ `setupapi_shim.c` | 1. 實作 `cfgmgr32.dll` 墊片處理 `0x3000 -> 0x2000 -> 0x1000` 的 `CM_Get_Parent` / `CM_Get_Child`。<br>2. `setupapi_shim.c` 擴充為三裝置表：`flags=0x16` 回三顆、`flags=0x6` (`"USB"`) 回 `0x2000` 與 `0x1000`。<br>3. `SetupDiEnumDeviceInterfaces` 將 `DevInst` 寫入 `iface->Reserved`，讓 `SetupDiGetDeviceInterfaceDetailA/W` 依 `iface->Reserved` 回傳 `ROOT_HUB` 或 `DEV` 路徑。<br>4. `SetupDiGetDeviceInstanceId*` 與 `SetupDiGetDeviceRegistryProperty*` 嚴格依 `info->DevInst` 分派，並補上 `SPDRP_ENUMERATOR_NAME (0x16) = "USB"`。 |
| **Step 3** | `setupapi_shim.c`（或 `winusb_shim.c`） | 在 `DllMain` 掛上 `DSWave.exe` 的 `KERNEL32!DeviceIoControl` IAT Hook，針對 `ROOT_HUB` handle 實作 `#pragma pack(1)` 的 `0x220408`、`0x220448`（檢查 `ConnectionIndex == addr`）與 `0x220410`（確保第一次呼叫 `*ret == 21`、第二次 `*ret == 12 + wTotalLength`）。 |
| **Step 4** | 整合驗證 | 以 `LIBUSB_DEBUG=4` 啟動 `DSWave.exe -l 5`，確認看到 `(bus: 1, addr: 6, depth: 1, port: 6)` → `WinUsb_Initialize` → UI 出現實體裝置。 |
