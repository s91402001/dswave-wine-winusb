# DSWave on Wine — WinUSB 墊片實作計畫

最後更新：2026-10-29
依據：`ds_reply2.md`、`argon_reply2.md`、libusb `windows_winusb.c`

---

## 目標

讓 `DSWave.exe` 在 Wine 11.20 下，透過本專案的使用者模式墊片，
經 Linux libusb 與 `2a0e:0080`（USB-based DSL Instrument v3）通訊。

成功門檻：**DSWave 能列出裝置，低速率可擷取**。

---

## 架構

```
DSWave.exe (libusb 靜態連結)
 ├─ SetupAPI (shim)      → 三顆假裝置：HCD 0x3000 / HUB 0x2000 / DEV 0x1000
 ├─ Cfgmgr32 (shim)      → CM_Get_Parent / CM_Get_Child 父子關係
 ├─ DeviceIoControl(IAT) → 只攔假 Hub handle 的 4 個 IOCTL
 └─ WinUSB (shim)        → 真正的 open / bulk / control
        └─ TCP 127.0.0.1:47123 → helper (Linux libusb) → /dev/bus/usb/008/006
```

---

## Step 0 — 環境清理與腳本（0.5 h）

1. 刪除 `C:\Program Files\DSWave\` 殘留的 `setupapi.dll` / `winusb.dll` / `WinUSB.dll`
   （DSWave 以絕對路徑載入 system32，但其他模組會按名稱載入，留著會混淆）。
2. `install.sh` 備份來源改為 `/opt/wine-devel/lib/wine/x86_64-windows/`，
   命名 `setupapi_wine.dll`、`winusb_wine.dll`、`cfgmgr32_wine.dll`；
   另複製 `setupapi_orig.dll`、`cfgmgr32_orig.dll`（給 forwarder 用）。
3. `install.sh` 加入 `cfgmgr32` 的覆蓋與 `DllOverrides=native`；
   `uninstall.sh` 加入還原。
4. `install.sh` 自動清理 app 目錄殘留 DLL。

驗收：registry 有 `setupapi`、`winusb`、`cfgmgr32` 三個 native override。

---

## Step 1 — `proto.h` / `helper.c`（1 h）

1. `struct dev_info` 擴充：

```c
struct dev_info {
    uint32_t bus, addr, vid, pid, speed, interfaces;
    uint32_t config_len;
    uint8_t  device_desc[18];
    uint8_t  config_desc[4096];
};
```

2. `do_list()`：對符合 `2a0e` 的裝置，暫時 `libusb_open`（**不 claim**）、
   讀 device descriptor 與 config descriptor（先讀 9 bytes 取得 `wTotalLength`）、
   立即 `libusb_close`。
3. `do_descriptor()`：改用 `libusb_control_transfer(..., wIndex=req->c, ...)`，
   修正 string descriptor 的 langid。
4. `REQ_LIST` 回傳含描述元的 `dev_info`。

驗收：`helper --selftest` 正常；Python 客戶端可取得 18-byte descriptor 與
`wTotalLength` 相符的 config descriptor。

---

## Step 2 — `cfgmgr32.dll` 墊片（2–3 h）

新檔 `src/cfgmgr32_shim.c`：

| 呼叫 | 回傳 |
|---|---|
| `CM_Get_Parent(0x1000)` | `0x2000` |
| `CM_Get_Parent(0x2000)` / `(0x3000)` | `CR_NO_SUCH_DEVNODE` |
| `CM_Get_Child(0x3000)` | `0x2000` |
| `CM_Get_Child(0x2000)` | `0x1000` |
| `CM_Get_Child(0x1000)` | `CR_NO_SUCH_DEVNODE` |
| 其他 | forward → `cfgmgr32_orig.dll` |

- `gen_def.py` 泛化為可指定 DLL / 目標名稱 / 已實作清單。
- 移除 `setupapi_shim.c` 內原本的 `CM_*` 實作（改由 cfgmgr32 負責）。

驗收：`CM_Get_Parent(0x1000)` 回 `0x2000`；`usbscan` 仍正常。

---

## Step 3 — `setupapi.dll` 三裝置表（3–4 h）

`src/setupapi_shim.c` 重構：

```c
struct fake_dev {
    DWORD devinst;              /* 0x3000 / 0x2000 / 0x1000 */
    const WCHAR *instance_id;
    const WCHAR *path;
    int parent;
    int is_root_hub;
    int n_ifaces;
    const GUID *ifaces[2];
};
```

| DevInst | Instance ID | 介面 | 屬性重點 |
|---|---|---|---|
| `0x3000` HCD | `PCI\VEN_8086&DEV_1E31&SUBSYS_00000000&REV_00\0` | `{3ABF6F2D}` | `ENUMERATOR_NAME="PCI"` |
| `0x2000` HUB | `USB\ROOT_HUB30\4&0&0` | `{F18A0E88}` | `ENUMERATOR_NAME="USB"`；`ADDRESS`/`LOCATION` 必須失敗 |
| `0x1000` DEV | `USB\VID_2A0E&PID_0080\8&6` | `{A5DCBF10}`, `{DEE824EF}` | `LOCATION="Port_#0006.Hub_#0008"`、`SERVICE="WinUSB"`、`INSTALL_STATE=0` |

- `flags=0x16`（介面集合）→ HCD + HUB + DEV
- `flags=0x6`（`"USB"`）→ HUB + DEV（HUB 在前）
- `SetupDiEnumDeviceInterfaces` 成功時 `iface->Reserved = devinst`
- `SetupDiGetDeviceInterfaceDetailA/W` 依 `iface->Reserved` 回路徑
- `SetupDiGetDeviceInstanceId*` / `GetDeviceRegistryProperty*` 依 `info->DevInst` 分派
- `SetupDiOpenDevRegKey` 依裝置建 Enum / Class 機碼

路徑：
- HCD：`\\?\C:\ProgramData\DSWaveShim\HCD.dev`
- HUB：`\\?\C:\ProgramData\DSWaveShim\ROOT_HUB`（必須可讀寫）
- DEV：`\\?\C:\ProgramData\DSWaveShim\USB#VID_2A0E&PID_0080#8&6#{A5DCBF10-...}`

驗收（V1）：`LIBUSB_DEBUG=4` 不再出現 `unlisted ancestor`，並看到
`assigning Root Hub ... bus number 1` 與
`could not get node connection information for device ...`。

---

## Step 4 — `DeviceIoControl` IAT hook（3–4 h）

新檔 `src/ioctl_hook.c`，由 `setupapi.dll` 的 `DllMain` 安裝
（僅當目前 exe 是 `DSWave.exe`）：

1. 掃 `IMAGE_DIRECTORY_ENTRY_IMPORT`，把 `KERNEL32!DeviceIoControl`
   換成 `hook_DeviceIoControl`，保存 trampoline。
2. 用 `GetFinalPathNameByHandleW` 判斷是否為 `ROOT_HUB` handle。

| IOCTL | 規則 |
|---|---|
| `0x220408` | `USB_NODE_INFORMATION`；`bNumberOfPorts=8`（offset 6）；`*ret=out_len` |
| `0x220448` | **pack(1)** 35 bytes；依 `ConnectionIndex` 填 descriptor、`CurrentConfigurationValue=1`、`Speed=3`、`DeviceAddress=6`、`NumberOfOpenPipes=3`、`ConnectionStatus=1` |
| `0x22045c` | **pack(1)** 16 bytes；`Length=16`、`SupportedUsbProtocols=0x04`、`Flags=0x03`（port 相符） |
| `0x220410` | 12-byte 標頭 + descriptor；dummy 回 9 bytes（`*ret=21`）；actual 回 `12+min(out_len-12, config_len)` |

- 有 `OVERLAPPED` 時同步完成 + `SetEvent`。

驗收（V2）：`(bus: 1, addr: 6, depth: 1, port: 6)` 且
`winusb.log` 出現 `Initialize: opened 2a0e:0080 ...`。

---

## Step 5 — 建置與安裝（1 h）

- `build.sh`：新增 `cfgmgr32.dll`；`gen_def.py` 支援多 DLL。
- `install.sh`：三顆墊片 + 三個 override + 清理 + 啟動 helper。
- `uninstall.sh`：完整還原。

---

## Step 6 — 驗證（2–4 h）

| 編號 | 條件 |
|---|---|
| V0 | 基準線：`unlisted ancestor` |
| V1 | 無 `unlisted ancestor`、見 Root Hub 與 Hub IOCTL 失敗訊息 |
| V2 | `(bus:1, addr:6, depth:1, port:6)` + `WinUsb_Initialize` |
| V3 | UI 出現裝置、讀到 `DreamSourceLab` / `USB-based DSL Instrument v3` |
| V4 | 低速率擷取成功 |

---

## 風險

| 風險 | 對策 |
|---|---|
| IAT hook 位置錯誤 | 掃 import directory；失敗即中止並記錄 |
| 結構對齊 | 全部 `#pragma pack(push,1)`；以 `out_len` 為上限 |
| `0x220410` 長度 | dummy 必須 `*ret=21`，actual 必須 `12+copy_len` |
| 三裝置共用 set | `iface->Reserved` 存 `DevInst` |
| 失敗需回滾 | `uninstall.sh` 還原三顆 DLL 與 override |

---

## 檔案變更

| 檔案 | 動作 |
|---|---|
| `src/proto.h` | 擴充 `dev_info` |
| `src/helper.c` | 描述元、langid |
| `src/cfgmgr32_shim.c` | 新增 |
| `src/setupapi_shim.c` | 三裝置表重構 |
| `src/ioctl_hook.c` | 新增 |
| `src/shim_internal.h` | 新增（共用宣告） |
| `scripts/gen_def.py` | 泛化 |
| `scripts/build.sh` / `install.sh` / `uninstall.sh` | 更新 |
