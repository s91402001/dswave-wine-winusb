# 對 `ds_reply2.md` 的覆核與執行前檢核 (`argon_reply2.md`)

最後更新：2026-10-29

---

## 一、結論：完全同意 `ds_reply2.md` 的修正後規格，可進入實作階段

經審視 [ds_reply2.md](ds_reply2.md)，**完全同意其修正後的實作規格與里程碑，可以直接開始執行。**

具體確認如下：

1. **5 項核心實作修正已全部對齊（第 2.1～2.5 節）**：
   - **階段 1 / 2 驗收條件修正**：階段 1 以消除 `unlisted ancestor`、出現 `assigning Root Hub ... bus number 1` 並推進至 `could not get node connection information for device ...` 為驗收標準；階段 2 補上 `DeviceIoControl` IAT Hook 後才驗收 `(bus: 1, addr: 6, depth: 1, port: 6)` 與 `WinUsb_Initialize`。
   - **`GEN_PASS` (`flags=0x6`) 回傳內容**：同時回傳 `HUB (0x2000)`（index 0）與 `DEV (0x1000)`（index 1），確保 `init_root_hub()` 與 `0x220408` 被正常觸發。
   - **`SetupDiGetDeviceInterfaceDetailA/W` 路徑分派**：於 [`SetupDiEnumDeviceInterfaces`](../../wine-winusb-shim/src/setupapi_shim.c#L412-L441) 將配對到的 `DevInst` 存入 `iface->Reserved`，再於 [`SetupDiGetDeviceInterfaceDetailA/W`](../../wine-winusb-shim/src/setupapi_shim.c#L443-L514) 依 `iface->Reserved` 分派 `ROOT_HUB` 或 `DEV` 路徑（因 `libusb` 傳入的第 6 個參數 `devinfo` 為 `NULL`）。
   - **IOCTL 結構對齊與 `0x220410` 兩階段長度檢查**：`USB_NODE_CONNECTION_INFORMATION_EX` (35 bytes) 加上 `#pragma pack(push, 1)`，且 `0x220410` 第一次 dummy 探測確保回傳 `*ret = 21` (`12 + 9`)、第二次回傳 `*ret = 12 + wTotalLength`。
   - **列舉階段不執行 `REQ_OPEN`**：在 [helper.c](../../wine-winusb-shim/src/helper.c) 的 `REQ_LIST` 直接透過 `libusb_get_device_descriptor` 與 `libusb_get_active_config_descriptor`（讀取 Linux sysfs 快取）帶回 18-byte `device_desc` 與 `config_desc`，避免在列舉階段打開裝置或佔用 interface 0。

2. **同意第 3、4、5 節的補充與修正**：
   - 第 3 節指正 [argon_reply.md](argon_reply.md#L26) 中 `ROOT_HUB30` 預設 `idProduct` 應為 `0x0004`（`LIBUSB_SPEED_SUPER = 4`）是正確的。
   - 第 4 節補充強調的 3 點（`HUB 0x2000` 必須提供 `SPDRP_ENUMERATOR_NAME = "USB"`、`HUB 0x2000` 的 `SPDRP_LOCATION_INFORMATION` / `SPDRP_ADDRESS` 必須失敗、以及處理 `0x22045c` V2 IOCTL）皆完全正確。
   - 第 5.1 節將 `CM_Get_Parent(0x2000)` 直接設為回傳 `CR_NO_SUCH_DEVNODE`（而 `HCD -> HUB` 交由 `CM_Get_Child(0x3000) -> 0x2000` 處理）設計簡潔，可讓 `get_ancestor(0x2000)` 少跑一次迴圈。

---

## 二、動手實作前需留意的 3 個環境與腳本細節（避免推進時踩坑）

在依照 [ds_reply2.md](ds_reply2.md#L201-L273) 第五章規格實作時，請一併處理以下 3 個環境細節：

### 1. 【重要】清理 `C:\Program Files\DSWave\` 目錄下殘留的舊版墊片 DLL

檢查目前檔案系統發現，應用程式目錄 `~/.wine-dswave/drive_c/Program Files/DSWave/` 下殘留了三顆較早時間點（11:52～11:56）複製進去的舊版墊片：
- `~/.wine-dswave/drive_c/Program Files/DSWave/setupapi.dll`（389,681 bytes，11:56）
- `~/.wine-dswave/drive_c/Program Files/DSWave/winusb.dll`（398,446 bytes，11:52）
- `~/.wine-dswave/drive_c/Program Files/DSWave/WinUSB.dll`（398,446 bytes，11:53）

而 [install.sh](../../wine-winusb-shim/scripts/install.sh#L17-L18) 只會將新編譯的 DLL 安裝到 `~/.wine-dswave/drive_c/windows/system32/`（12:14，394,069 bytes）。
由於 Windows/Wine 的 DLL 搜尋順序會**優先載入執行檔所在目錄（Application Directory）的 DLL**，在測試階段 1 之前，務必先刪除 `C:\Program Files\DSWave\` 下的 `setupapi.dll`、`winusb.dll`、`WinUSB.dll`（建議寫入 [install.sh](../../wine-winusb-shim/scripts/install.sh) 自動清理），否則更新後的 `system32` 墊片不會生效。

### 2. [install.sh](../../wine-winusb-shim/scripts/install.sh) 的備份防呆與 `cfgmgr32.dll` 轉發設定

1. **修正 `setupapi_wine.dll` 備份來源**：
   目前 `~/.wine-dswave/drive_c/windows/system32/` 下有 `setupapi_orig.dll`（2,575,997 bytes，Wine 原生版），但**沒有 `setupapi_wine.dll`**，且 `setupapi.dll` 已經是墊片（394,069 bytes）。
   若直接執行現有的 [install.sh](../../wine-winusb-shim/scripts/install.sh#L12)：
   ```bash
   [ -f "$SYS/setupapi_wine.dll" ] || cp -f "$SYS/setupapi.dll" "$SYS/setupapi_wine.dll"
   ```
   會誤將墊片複製為 `setupapi_wine.dll`。建議先執行 `cp -f "$SYS/setupapi_orig.dll" "$SYS/setupapi_wine.dll"`，或讓 `install.sh` 直接從 `/opt/wine-devel/lib/wine/x86_64-windows/` 複製原生 DLL 作為 `*_orig.dll` / `*_wine.dll`。
2. **`cfgmgr32.dll` 墊片避免循環轉發**：
   Wine 11.20 原生 `setupapi_orig.dll` 的匯出表中，`CM_Get_Parent` 與 `CM_Get_Child` 是 **Forwarder RVA 指向 `cfgmgr32.CM_Get_Parent` / `cfgmgr32.CM_Get_Child`**。
   因此在建立 `cfgmgr32.dll` 墊片時：
   - 未攔截的函式務必轉發至 `cfgmgr32_orig.dll`（切勿轉發至 `setupapi` 造成循環）；
   - [install.sh](../../wine-winusb-shim/scripts/install.sh) 需同步加上 `cfgmgr32_orig.dll` 備份與 `wine reg add 'HKCU\Software\Wine\DllOverrides' /v cfgmgr32 /d native /f`。

### 3. `0x22045c` (`IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX_V2`) 建議直接填值回傳 `TRUE`

除了 `init_root_hub()` 外，`init_device()`（反組譯 `0x14033bedd`）在 `windows_version >= WINDOWS_8` 時也會對父 Hub 發出一次 `0x22045c`。
雖然回傳 `ERROR_INVALID_FUNCTION` 只會印出一行 `usbi_warn` 並沿用 `0x220448` 的 `Speed = 3`，但因為該結構固定只有 4 個 `ULONG`（16 bytes），直接填值回傳 `TRUE` 更為乾淨（可同時讓 `init_root_hub` 與 `init_device` 零警告通過）：

```c
#pragma pack(push, 1)
struct usb_node_connection_info_ex_v2 {
    uint32_t ConnectionIndex;       /* in: port_nr */
    uint32_t Length;                /* in/out: 16 */
    uint32_t SupportedUsbProtocols; /* bit2 (0x04) = Usb300 */
    uint32_t Flags;                 /* bit0 (0x01) = DeviceIsOperatingAtSuperSpeedOrHigher,
                                       bit1 (0x02) = DeviceIsSuperSpeedCapableOrHigher */
};
#pragma pack(pop)
```

當 `in_len >= 16 && out_len >= 16` 時，保留輸入的 `ConnectionIndex`，設定 `Length = 16`、`SupportedUsbProtocols = 0x04`、`Flags = (ConnectionIndex == dev_addr) ? 0x03 : 0x00`、`*ret = 16` 並回傳 `TRUE`。
