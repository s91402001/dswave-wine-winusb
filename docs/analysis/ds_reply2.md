# 對 `argon_reply.md` 的覆核與計畫修正 (`ds_reply2.md`)

最後更新：2026-10-29
狀態：覆核完成，計畫已修正，等待執行

---

## 一、摘要

`argon_reply.md` 對 `ds_reply.md` 提出的 5 點修正 **全部成立**，其中 4 點是我原本的實作細節錯誤、1 點是驗收條件寫反。本文逐條以 libusb 上游原始碼驗證後接受，並補上 3 個它沒有強調、但同樣會卡住的點。

另外發現 `argon_reply.md` 本身有 1 處小錯誤（root hub 的 `idProduct`）。

---

## 二、逐條驗證（以 libusb master 原始碼為準）

### 2.1 階段 1／2 的驗收條件（我錯）

**原 `ds_reply.md` 寫法**
- 階段 1：`LIBUSB_DEBUG` 出現 `(bus:1, addr:6, depth:2)`，`libusb_get_device_list` 回 1
- 階段 2：DSWave 呼叫 `WinUsb_Initialize`

**實際行為**（`windows_winusb.c`）

`init_device()`（L1386 起）在 `parent_dev != NULL` 時會：
1. `CreateFileA(parent_priv->path, GENERIC_WRITE, FILE_SHARE_WRITE, ..., FILE_FLAG_OVERLAPPED)`
2. `IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX`（`0x220448`）
3. `cache_config_descriptors()` → 兩次 `IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION`（`0x220410`）
4. 最後才 `usbi_dbg("(bus: %u, addr: %u, depth: %u, port: %u) ...")`
5. `GEN_PASS` 才 `discovered_devs_append()`

沒有 IOCTL hook 時，第 2 步必然失敗 → 印
`could not get node connection information for device '%s'` → 裝置被丟棄。

**修正**
- 階段 1 驗收：
  1. `LIBUSB_DEBUG=4` 不再出現 `unlisted ancestor for 'USB\VID_2A0E&PID_0080\8&6'`
  2. 出現 `assigning Root Hub 'USB\ROOT_HUB30\4&0&0' bus number 1`
  3. 出現 `could not get node connection information for device 'USB\VID_2A0E&PID_0080\8&6'`
     （代表父節點關係已通，只差 IOCTL）
- 階段 2 驗收：出現 `(bus: 1, addr: 6, depth: 1, port: 6)` → `WinUsb_Initialize`。

**depth 更正**：root hub 的 `priv->depth` 為 0（未設定），裝置為
`parent_priv->depth + 1 = 1`，不是 2。

---

### 2.2 `GEN_PASS`（`"USB"` 列舉）必須同時回 HUB 與 DEV

**原 `ds_reply.md` 寫法**：`flags=0x6` 只回 DEV。

**實際行為**：`init_root_hub()` 不在 `HUB_PASS` 呼叫。
`GEN_PASS` 對每個節點先做 `get_ancestor()`：

```c
// L2217-2231
if (pass >= GEN_PASS) {
    parent_dev = get_ancestor(ctx, dev_info_data.DevInst, NULL);
    if (parent_dev == NULL) {
        dev = usbi_get_device_by_session_id(ctx, dev_info_data.DevInst);
        if (dev != NULL) {
            priv = ...;
            if (priv->root_hub)
                goto track_unref;      // ← root hub 走這
            libusb_unref_device(dev);
        }
        usbi_dbg(ctx, "unlisted ancestor ... - ignoring");
        continue;
    }
}
```

`goto track_unref` 之後進入 `GEN_PASS` 的 `init_device(dev, parent_dev = NULL, ...)`
→ `init_root_hub()`（L1386 的 `if (parent_dev != NULL) ... else init_root_hub(dev)`）。

**修正**：`SetupDiGetClassDevsA(NULL, "USB", flags=0x6)` 必須回
`HUB 0x2000`（index 0）與 `DEV 0x1000`（index 1）。
否則 `IOCTL_USB_GET_NODE_INFORMATION (0x220408)` 永遠不會被呼叫，
root hub 的 `priv->initialized` 一直是 0。

（註：DEV 本身仍可初始化，因為 `get_ancestor` 只查 session id；
但 root hub 不初始化會讓整個樹不完整，且 `0x220408` 的驗證失去意義，
所以仍應回傳。）

---

### 2.3 `SetupDiGetDeviceInterfaceDetail*` 的 `devinfo` 為 `NULL`

**實際行為**：libusb 呼叫

```c
SetupDiGetDeviceInterfaceDetailA(set, &dev_interface_data, detail, size, &required, NULL);
```

第 6 個參數固定傳 `NULL`。而現有 `setupapi_shim.c` 的
`SetupDiGetDeviceInterfaceDetailA/W` 有 `(void)iface;`，完全沒看第 2 個參數。

三顆裝置共用同一個 `HDEVINFO` 時，detail 無法知道要回哪一顆的路徑。

**修正**
1. `SetupDiEnumDeviceInterfaces` 成功時把該裝置的 `DevInst` 寫入
   `iface->Reserved`（`ULONG_PTR`）。
2. `SetupDiGetDeviceInterfaceDetailA/W` 依 `iface->Reserved` 選裝置路徑。
3. `SetupDiGetDeviceInstanceId*`、`SetupDiGetDeviceRegistryProperty*`
   一律依 `info->DevInst` 分派（不可再 `(void)info;`）。
4. HUB（`0x2000`）查詢 `SPDRP_ADDRESS` / `SPDRP_LOCATION_INFORMATION`
   必須回失敗（或 0），否則 `HUB_PASS` 會把它當成非 root hub，
   `bus_number` 停在 0。

---

### 2.4 `USB_NODE_CONNECTION_INFORMATION_EX` 必須 pack，`0x220410` 第一次 `*ret` 必須是 21

**結構（Windows `usbioctl.h` 以 `pshpack1.h` 包住，共 35 bytes）**

| offset | 大小 | 欄位 | 值 |
|---|---|---|---|
| `+0x00` | 4 | `ConnectionIndex` | 由輸入帶入（本機為 6）；不符時回 `ConnectionStatus=0` |
| `+0x04` | 18 | `DeviceDescriptor` | 由 helper 提供（`bLength=0x12`, `bDescriptorType=0x01`） |
| `+0x16` | 1 | `CurrentConfigurationValue` | `1` |
| `+0x17` | 1 | `Speed` | `3`（`UsbSuperSpeed`） |
| `+0x18` | 1 | `DeviceIsHub` | `0` |
| `+0x19` | 2 | `DeviceAddress` | `6`（**未對齊**） |
| `+0x1B` | 4 | `NumberOfOpenPipes` | `3`（**未對齊**） |
| `+0x1F` | 4 | `ConnectionStatus` | `1`（`DeviceConnected`，**未對齊**） |

未加 `#pragma pack(push,1)` 會插入 padding，`DeviceAddress` 與
`ConnectionStatus` 位移全錯。

**`0x220410` 兩階段語意**（`cache_config_descriptors()` L1030-1130）

1. **dummy 呼叫**：輸入/輸出長度 `size = sizeof(USB_DESCRIPTOR_REQUEST) + 9 = 21`。
   原始碼硬檢查 `ret_size != size` → 直接 `continue`（放棄該設定）。
2. **actual 呼叫**：`size = 12 + wTotalLength`，且必須
   `ret_size == size`、`cd_data->bDescriptorType == 2`（`USB_DT_CONFIG`）。

**修正**
- 輸出先複製輸入的 12-byte `USB_DESCRIPTOR_REQUEST` 標頭
  （`ConnectionIndex` + 8-byte `SetupPacket`）。
- `copy_len = min(out_len - 12, full_config_len)`。
- **一律** `*ret = 12 + copy_len`。
- dummy 呼叫時 `full_config_len` 取前 9 bytes，確保 `*ret == 21`。

---

### 2.5 Hub IOCTL 不宜走既有 `REQ_DESCRIPTOR`

**現況**：`helper.c` 的 `do_descriptor()` 需要有效 `req->handle`（先 `REQ_OPEN`），
而 `REQ_OPEN` 會 `libusb_open()` + `libusb_claim_interface(h, 0)`。

列舉階段還沒有 `WinUsb_Initialize`，若為回答 Hub IOCTL 而 `REQ_OPEN`：
- 會佔用 interface 0；
- 反覆掃描會消耗 `MAX_HANDLES (8)`；
- 每次掃描都做一次無謂的 claim/release。

**修正**：在 `REQ_LIST` 的 `struct dev_info` 直接附帶：
- 18-byte device descriptor（`libusb_get_device_descriptor`）
- 完整 config descriptor（`libusb_get_active_config_descriptor`）

Linux 上這兩者只讀 sysfs，**不需要 open / claim**。
只有 DSWave 真正呼叫 `WinUsb_Initialize` 時才做一次 `REQ_OPEN`。

---

## 三、`argon_reply.md` 的一處小錯

第 26 行：

> `init_root_hub()` … 填寫 USB 3.0 Root Hub 預設描述元（`0x1d6b:0x0003`）

原始碼（L1327-1330）：

```c
dev->device_descriptor.idVendor = 0x1d6b;      /* Linux Foundation */
dev->device_descriptor.idProduct = (uint16_t)speed;
```

`ROOT_HUB30` 時 `speed = LIBUSB_SPEED_SUPER = 4`，所以是 **`0x1d6b:0x0004`**。

不影響本專案（root hub 的 VID/PID 不參與 DSWave 的裝置過濾），但文件要寫對。

---

## 四、`argon_reply.md` 沒強調、但同樣會卡住的 3 點

1. **`SPDRP_ENUMERATOR_NAME (0x16)` 必須給 HUB**
   `HUB_PASS` 會讀它；讀不到就
   `usbi_err("could not read enumerator string ...")` + `LOOP_BREAK(LIBUSB_ERROR_OTHER)`，
   整個列舉直接中止。
2. **HUB 的 `SPDRP_LOCATION_INFORMATION` / `SPDRP_ADDRESS` 必須失敗**
   `get_dev_port_number()` 若能取出 port > 0，HUB 就不會被判定為 root hub，
   `dev->bus_number` 停在 0，後續 `init_device` 會 `bus_number not found`。
3. **Windows 10 會先跑 `0x22045c`（V2）**
   `init_root_hub()` 在 `windows_version >= WINDOWS_8` 時先逐 port 查 V2。
   讓它回 `ERROR_INVALID_FUNCTION` 即可（libusb 會 break 後改用
   `ROOT_HUB30` 名稱判定 SuperSpeed）；要更穩就直接回
   `DeviceIsOperatingAtSuperSpeedOrHigher`。

---

## 五、修正後的實作規格

### 5.1 `cfgmgr32.dll` 墊片（新增）

| 呼叫 | 回傳 |
|---|---|
| `CM_Get_Parent(0x1000)` | `0x2000`（HUB），`CR_SUCCESS` |
| `CM_Get_Parent(0x2000)` | `CR_NO_SUCH_DEVNODE` |
| `CM_Get_Parent(0x3000)` | `CR_NO_SUCH_DEVNODE` |
| `CM_Get_Child(0x3000)` | `0x2000`（`enumerate_hcd_root_hub` 需要） |
| `CM_Get_Child(0x2000)` | `0x1000` |
| 其他 | forward 回 `cfgmgr32_orig.dll` |

### 5.2 `setupapi.dll` 墊片：三裝置表

| DevInst | Instance ID | 介面 GUID | 必要屬性 |
|---|---|---|---|
| `0x3000` HCD | `PCI\VEN_8086&DEV_1E31&SUBSYS_00000000&REV_00\0` | `{3ABF6F2D-71C4-462A-8A92-1E6861E6AF27}` | 能被 `PCI\VEN_%04hx&DEV_%04hx` 解析 |
| `0x2000` HUB | `USB\ROOT_HUB30\4&0&0` | `{F18A0E88-C30C-11D0-8815-00A0C906BED8}` | `ENUMERATOR_NAME="USB"`；**無** port、**無** parent；路徑指向可讀寫檔案 `ROOT_HUB` |
| `0x1000` DEV | `USB\VID_2A0E&PID_0080\8&6` | `{A5DCBF10-6530-11D2-901F-00C04FB951ED}`、`{DEE824EF-729B-4A0E-9C14-B7117D33A817}` | `LOCATION_INFORMATION="Port_#0006.Hub_#0008"`、`SERVICE="WinUSB"`、`INSTALL_STATE=0`、`DRIVER="{36fc9e60-...}\0001"` |

`SetupDiGetClassDevsA` 兩個變體：

| 呼叫 | 回傳 |
|---|---|
| `(NULL, NULL, DIGCF_PRESENT\|DIGCF_DEVICEINTERFACE)` = `flags 0x16` | HCD + HUB + DEV |
| `(NULL, "USB", DIGCF_PRESENT\|DIGCF_ALLCLASSES)` = `flags 0x6` | HUB + DEV（順序：HUB 先） |
| 其他 GUID | 依 `is_usb_guid` 判斷，其餘 forward |

### 5.3 IAT hook `DeviceIoControl`

在 `setupapi.dll` 的 `DllMain`（或第一次 `SetupDiGetClassDevs`）時，
以 `ImageDirectoryEntryToData` 找到 `DSWave.exe` 的 KERNEL32 import thunk，
用 `VirtualProtect` 換掉 `DeviceIoControl` 的指標。

判斷 handle 是否為假 Hub（`GetFinalPathNameByHandleW` 含 `ROOT_HUB`）：

| IOCTL | 代碼 | 回應 |
|---|---|---|
| `IOCTL_USB_GET_NODE_INFORMATION` | `0x220408` | `USB_NODE_INFORMATION`，`bNumberOfPorts=8`（輸入可為 NULL） |
| `IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX` | `0x220448` | 35-byte packed 結構（見 2.4） |
| `IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX_V2` | `0x22045c` | 回 `ERROR_INVALID_FUNCTION`，或填 SuperSpeed flags |
| `IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION` | `0x220410` | 12-byte 標頭 + descriptor，`*ret` 規則見 2.4 |
| 其他 handle / code | — | 轉回原始 `DeviceIoControl` |

有 `LPOVERLAPPED` 時：同步完成、填 `Internal/InternalHigh`、`SetEvent(hEvent)`、回 `TRUE`。

### 5.4 helper 變更

1. `do_descriptor()` 改用：

```c
libusb_control_transfer(handles[id], LIBUSB_ENDPOINT_IN,
    LIBUSB_REQUEST_GET_DESCRIPTOR,
    (uint16_t)((req->a << 8) | req->b),   /* wValue */
    (uint16_t)req->c,                      /* wIndex = langid */
    buf, (uint16_t)req->len, 5000);
```

2. `REQ_LIST` 回傳 `struct dev_info` 擴充：

```c
struct dev_info {
    uint32_t bus, addr, vid, pid, speed, interfaces;
    uint8_t  device_desc[18];
    uint32_t config_len;
    uint8_t  config_desc[4096];   /* 或分成第二段 payload */
};
```

（列舉階段完全不 `REQ_OPEN`、不 claim interface。）

---

## 六、修正後里程碑

| 階段 | 工作 | 驗收 | 估時 |
|---|---|---|---|
| **0** | `LIBUSB_DEBUG=4` 現況確認 | 看到 `unlisted ancestor`（基準線） | 0.5 h |
| **1** | `cfgmgr32` 墊片 + 三裝置表 + 屬性分派 + `iface->Reserved` | 不再 `unlisted ancestor`；看到 `assigning Root Hub ... bus number 1` 與 `could not get node connection information ...` | 0.5–1 d |
| **2** | IAT hook + 4 個 IOCTL + helper `REQ_LIST` 帶描述元 | `(bus: 1, addr: 6, depth: 1, port: 6)`；`WinUsb_Initialize` 出現 | 0.5–1 d |
| **3** | langid 修正、字串驗證、claim interface | DSWave 出現實體裝置 tab | 0.5 d |
| **4** | 低速率擷取 | 低取樣率可擷取、波形正確 | 0.5–1 d |

**總計 1.5–3 個工作天。**

---

## 七、風險

| 風險 | 對策 |
|---|---|
| IAT hook 位址/大小錯誤 | 用 `ImageDirectoryEntryToData` 掃描，只改一項；失敗則不 hook 並記錄 |
| 結構對齊 | 全部 IOCTL 結構用 `#pragma pack(push,1)`；以 `out_len` 為上限 |
| V2 IOCTL 行為 | 先回 `ERROR_INVALID_FUNCTION`；若 libusb 因此判定失敗再補實作 |
| 重疊 I/O | hook 內同步完成 + `SetEvent` |
| 三裝置共用 set 的路徑錯亂 | `iface->Reserved` 存 `DevInst`，detail 依它分派 |
| libusb 版本差異 | 已對照 master；DSWave 內字串吻合 |
| 最終仍失敗 | 以 `LIBUSB_DEBUG=4` 下一個錯誤定位；備案為 VM `usb-host` 直通 |

---

## 八、與前兩份文件的差異

| 項目 | `argon_advise.md` | `ds_reply.md`（我） | 本文（修正後） |
|---|---|---|---|
| 路線 | 建議 A/B/C | 採 C 最小集合 | 採 C 最小集合（不變） |
| `DeviceInterfaceGUIDs` | 列為必要斷點 | 判為非必要 | 非必要（只影響 `EXT_PASS`） |
| `GEN_PASS` 回傳內容 | 未提 | 只回 DEV（**錯**） | HUB + DEV |
| 階段 1 驗收 | 未提 | `get_device_list` 回 1（**錯**） | 不再 `unlisted ancestor` + Hub IOCTL 失敗訊息 |
| depth | 未提 | 2（**錯**） | 1 |
| detail 的 `devinfo` | 未提 | 未處理（**漏**） | `iface->Reserved` 分派 |
| IOCTL 結構 | 未提 | 未提（**漏**） | `#pragma pack(1)`、35 bytes、`*ret==21` |
| 列舉階段 descriptor 來源 | 未提 | 用 `REQ_DESCRIPTOR`（**錯**） | `REQ_LIST` 直接帶回 |
| root hub `idProduct` | — | — | `0x0004`（`argon_reply.md` 寫 0x0003 有誤） |

---

## 九、目前系統狀態（承接 `progress.md`）

- `~/.wine-dswave` 已套用墊片：`setupapi=native`、`winusb=native`，
  system32 備份為 `setupapi_wine.dll` / `winusb_wine.dll`，另有 `setupapi_orig.dll`
- helper：`wine-winusb-shim/build/helper`（執行中）
- `usbscan.exe` 端到端通過（列舉 → CreateFile → WinUsb_Initialize → 3 endpoints）
- DSWave 掃描已打到墊片，卡在 `libusb_get_device_list()` 內部
- 裝置在 host、VM 關機中
