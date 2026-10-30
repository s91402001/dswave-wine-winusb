# 使用反組譯（Disassembly）逆向確認 USB 通訊協定與撰寫 Wine WinUSB Shim / Helper 實戰指南

本文件詳細說明在本專案（讓 Windows 64-bit `DSWave.exe` 透過 Wine + 自製 WinUSB 墊片與 Linux `libusb` Helper 控制 DreamSourceLab 硬體）中，如何透過 **PE 執行檔與 Wine 系統 DLL 的反組譯分析**，精確推導出：
1. **墊片（[`winusb_shim.c`](../../wine-winusb-shim/src/winusb_shim.c)、[`ioctl_hook.c`](../../wine-winusb-shim/src/ioctl_hook.c)、[`setupapi_shim.c`](../../wine-winusb-shim/src/setupapi_shim.c)）與 [`helper.c`](../../wine-winusb-shim/src/helper.c) 的實作契約**（應該攔截哪些 API、填入什麼狀態碼、何時取消傳輸）。
2. **硬體 USB 通訊格式與狀態機**（Header 與 Data 封包長度檢查、控制傳輸握手順序、Drain/Stop 模式行為）。

---

## 一、為什麼單看文件與 Log 不夠，必須反組譯？

在開發 Wine 墊片與硬體 Helper 時，常會遇到以下「黑盒子」困境：
1. **公開文件與實際行為不符**：例如微軟文件寫 WinUSB 取消傳輸用 `WinUsb_AbortPipe`，但 `DSWave.exe` 內嵌的 `libusb` 卻先呼叫 `KERNEL32!CancelIoEx`，且在收到特定錯誤碼時直接放棄呼叫 `WinUsb_AbortPipe`。
2. **App 靜默吞掉錯誤（Silent Failure）**：當某個呼叫回傳特定錯誤碼（例如 `-5 = LIBUSB_ERROR_NOT_FOUND`）時，App 內部直接 `je` 跳過不印 Log，導致從 Log 完全看不出問題。
3. **App 的錯誤訊息誤導**：例如 App 印出 `Data transfer failed: LIBUSB_SUCCESS / LIBUSB_TRANSFER_COMPLETED; length: 0`，只有反組譯才能確認這究竟是 `transfer->status` 真的為 `0`，還是 App 呼叫錯了錯誤轉字串函式。
4. **硬體封包格式缺乏規格書**：Vendor Specific (`0xFF`) 的 USB 裝置沒有公開規格書，必須從 App 的 Callback 反組譯中看出它如何檢查封包長度、Magic Word (`0x55555555`) 與狀態旗標。

---

## 二、基礎工具鏈與 PE 位址換算公式（VMA ↔ File Offset）

在 Linux 下分析 64-bit Windows PE 檔（`x86_64-w64-mingw32` / `pei-x86-64`），只需要 `objdump`、`strings` 與 `python3` 三個工具。

### Step 1：讀取 PE Section 表，建立「檔案偏移 ↔ 虛擬位址（VMA）」換算公式

執行 `objdump -h` 查看 `DSWave.exe` 的區段映射：

```bash
objdump -h "~/.wine-dswave/drive_c/Program Files/DSWave/DSWave.exe"
```

關鍵輸出如下：
```text
Idx Name          Size      VMA               LMA               File off  Algn
  0 .text         00ac4d48  0000000140001000  0000000140001000  00000400  2**4
  1 .data         00013420  0000000140ac6000  0000000140ac6000  00ac5200  2**4
  2 .rdata        003de4e0  0000000140ada000  0000000140ada000  00ad8800  2**4
  8 .idata        00019378  000000014108c000  000000014108c000  00fcc600  2**4
```

由此得出萬用換算公式：
$$\text{VMA} = \text{File Offset} - \text{Section File off} + \text{Section VMA}$$

以唯讀資料區 `.rdata`（存放所有字串、函式指標表、常數結構）為例：
$$\text{VMA}_{\text{.rdata}} = \text{File Offset} - \mathtt{0xad8800} + \mathtt{0x140ada000} = \text{File Offset} + \mathtt{0x140001800}$$

### Step 2：由字串計算 VMA 並找出交叉引用（Xref）

在 `x86_64` 中，程式碼引用字串或全域變數幾乎都使用 **RIP-relative 尋址**（例如 `lea 0x8a6be0(%rip), %rdx`）。`objdump -d` 會在行尾自動幫我們算出目標的絕對 VMA（顯示為 `# 0x140b5b8d0`）！

因此，只要寫三行 Python 找到字串的 File Offset，加上 `0x140001800` 得到 VMA，再用 `grep` 搜尋 `objdump -d` 的輸出，就能在幾秒內定位到使用該字串的函式：

```bash
python3 -c '
data = open("~/.wine-dswave/drive_c/Program Files/DSWave/DSWave.exe", "rb").read()
for s in [b"Data transfer failed:", b"libusb_cancel_transfer\x00", b"winusbx_cancel_transfer\x00"]:
    idx = data.find(s)
    print(s.decode().strip("\x00"), "FileOff:", hex(idx), "-> VMA:", hex(idx + 0x140001800))
'
```
輸出：
```text
Data transfer failed:   FileOff: 0xb5a0d0 -> VMA: 0x140b5b8d0
libusb_cancel_transfer  FileOff: 0xdc59a0 -> VMA: 0x140dc71a0
winusbx_cancel_transfer FileOff: 0xdca760 -> VMA: 0x140dcbf60
```

接著用 `objdump -d` 搭配 `--start-address` 與 `--stop-address`（只反組譯目標區間，耗時不到 0.1 秒）：

```bash
# 第一次先 grep 找出引用該 VMA 的指令位址
objdump -d "~/.wine-dswave/drive_c/Program Files/DSWave/DSWave.exe" | grep "140b5b8d0" -B 20 -A 20

# 知道函式位址範圍後，用 --start-address / --stop-address 瞬間反組譯整個函式
objdump -d --start-address=0x1402b4c90 --stop-address=0x1402b4d10 \
  "~/.wine-dswave/drive_c/Program Files/DSWave/DSWave.exe"
```

### Step 3：認識 Windows x64 呼叫慣例（Microsoft x64 Calling Convention）

閱讀 `DSWave.exe` 與 Wine 64-bit DLL 反組譯時，請牢記前 4 個參數的暫存器順序（與 Linux System V x86_64 的 `rdi, rsi, rdx, rcx` 不同！）：
- **`%rcx`（或 32-bit `%ecx`）**：第 1 個參數（若是 C++ 成員函式，則為 `this` 指標）
- **`%rdx`（或 32-bit `%edx`）**：第 2 個參數
- **`%r8`（或 32-bit `%r8d`）**：第 3 個參數
- **`%r9`（或 32-bit `%r9d`）**：第 4 個參數
- **`0x20(%rsp)`、`0x28(%rsp)` ...**：第 5、第 6 個參數（前 `0x20` bytes 為 Shadow Space）
- **`%rax`（或 32-bit `%eax`）**：函式回傳值

---

## 三、實戰一：如何用反組譯確認 `helper` 與 Shim 的實作細節

### 案例 1：追查「為什麼按下 Stop 時，`helper` 從未收到 `REQ_ABORT_PIPE`？」

#### 1. 問題背景
在 [`winusb_shim.c`](../../wine-winusb-shim/src/winusb_shim.c#L919-L947) 中，我們實作了 [`WinUsb_AbortPipe()`](../../wine-winusb-shim/src/winusb_shim.c#L919-L947) 向 `helper` 發送 `REQ_ABORT_PIPE`。但搜尋整份 [`winusb.log`]($WINEPREFIX/drive_c/ProgramData/DSWaveShim/winusb.log)，卻發現 `AbortPipe` 出現次數是 **0 次**！

#### 2. 反組譯追蹤步驟

**第一步：定位 `libusb_cancel_transfer` (`0x14032e990`)**
透過字串 `"libusb_cancel_transfer"` 的 VMA `0x140dc71a0`，找到函式 `0x14032e990`：
```assembly
14032e9ef: 48 8b 05 7a c0 ad 00  mov  0xadc07a(%rip),%rax  # 載入 usbi_backend 指標 (0x140e0aa70)
14032e9f6: 48 89 f1              mov  %rsi,%rcx            # 參數 1: struct usbi_transfer *itransfer
14032e9f9: ff 90 e8 00 00 00     call *0xe8(%rax)          # 呼叫 usbi_backend->cancel_transfer
```

**第二步：解開 C 結構體函式指標表（Function Pointer Table）**
`0x140e0aa70` 指向 `.rdata` 中的 `windows_backend` 結構體，我們直接用 Python 讀取該結構體偏移 `+0xe8` 的函式指標：
```bash
python3 -c '
import struct
data = open("~/.wine-dswave/drive_c/Program Files/DSWave/DSWave.exe", "rb").read()
backend_vma = struct.unpack("<Q", data[0x140e0aa70 - 0x140001800 :][:8])[0]
cancel_fn = struct.unpack("<Q", data[backend_vma - 0x140001800 + 0xe8 :][:8])[0]
print("windows_backend:", hex(backend_vma), "cancel_transfer:", hex(cancel_fn))
'
# 輸出: windows_backend: 0x140dc8100 cancel_transfer: 0x140331c90
```

**第三步：反組譯 `windows_cancel_transfer` (`0x140331c90`) —— 發現真兇！**
```assembly
0000000140331c90 <windows_cancel_transfer>:
   ...
   140331cac: 48 8b 53 78           mov  0x78(%rbx),%rdx      # %rdx = overlapped (參數 2)
   140331cb0: 48 8b 4a 20           mov  0x20(%rdx),%rcx      # %rcx = hFile (參數 1)
   140331cb4: ff 15 32 0c d6 00     call *0xd60c32(%rip)      # 呼叫 IAT 0x1410928ec = KERNEL32!CancelIoEx
   140331cba: 31 d2                 xor  %edx,%edx
   140331cbc: 85 c0                 test %eax,%eax            # 若 CancelIoEx 回傳 TRUE，直接回傳 0 (成功)
   140331cbe: 74 10                 je   0x140331cd0          # 若回傳 FALSE，跳到 0x140331cd0
   ...
   140331cd0: ff 15 16 0d d6 00     call *0xd60d16(%rip)      # 呼叫 KERNEL32!GetLastError()
   140331cd6: 3d 90 04 00 00        cmp  $0x490,%eax          # 比對 GetLastError() == 0x490 (1168 = ERROR_NOT_FOUND)
   140331cdb: 74 4e                 je   0x140331d2b          # 若相等，跳到 0x140331d2b 直接回傳 -5 (LIBUSB_ERROR_NOT_FOUND)！
   140331cdd: 48 8b 86 00 02 00 00  mov  0x200(%rsi),%rax
   140331ce4: 48 8b 80 88 00 00 00  mov  0x88(%rax),%rax      # 載入 winusbx_cancel_transfer (內部呼叫 WinUsb_AbortPipe)
   ...
   140331cfa: 48 ff e0              rex.W jmp *%rax           # 只有 GetLastError() != 0x490 才會跳去呼叫 WinUsb_AbortPipe！
```

**第四步：確認 App 為何沒有報錯（反組譯 `0x1402b40b0` Drain Mode 函式）**
查詢誰呼叫了 `0x14032e990` (`libusb_cancel_transfer`)，在 `0x1402b4170` 看到：
```assembly
   1402b4173: e8 18 a8 07 00        call 0x14032e990          # eax = libusb_cancel_transfer(xfer)
   1402b4178: 85 c0                 test %eax,%eax
   1402b417a: 74 1b                 je   0x1402b4197          # 0 (成功) -> 跳過不報錯
   1402b417c: 83 f8 fb              cmp  $0xfffffffb,%eax     # -5 (LIBUSB_ERROR_NOT_FOUND) -> 也跳過不報錯！
   1402b417f: 74 16                 je   0x1402b4197
```

#### 3. 從反組譯推導出的實作結論
- 因為墊片開啟的 `hFile` 是假檔案 `dev.bin`，在 `wineserver` 中沒有核心級 I/O，所以 Wine 的 `CancelIoEx` 回傳 `FALSE` 且 `LastError = 0x490 (ERROR_NOT_FOUND)`。
- 這導致 `libusb` 誤判傳輸已結束而回傳 `-5`，且 App 刻意忽略 `-5`，於是 [`WinUsb_AbortPipe`](../../wine-winusb-shim/src/winusb_shim.c#L919-L947) 永遠不會被呼叫！
- **指導實作**：在 [`ioctl_hook.c`](../../wine-winusb-shim/src/ioctl_hook.c#L308-L338) 的 IAT Hook 中攔截 `CancelIoEx`，設 `SetLastError(ERROR_INVALID_FUNCTION)`（非 `0x490`）並回傳 `FALSE`，就能迫使 `0x140331cfa` 自動跳轉呼叫 `WinUsb_AbortPipe` → 向 `helper` 發送 `REQ_ABORT_PIPE`！

---

### 案例 2：確認 IOCP 完成通知與 `OVERLAPPED.Internal` 狀態碼映射

#### 1. 問題背景
當 [`helper.c`](../../wine-winusb-shim/src/helper.c) 的 Bulk 傳輸因超時、取消或 `-EPROTO` 結束時，[`winusb_shim.c`](../../wine-winusb-shim/src/winusb_shim.c#L312-L330) 的 [`finish_bulk()`](../../wine-winusb-shim/src/winusb_shim.c#L312-L330) 寫入 `ov->Internal = status` 並呼叫 `PostQueuedCompletionStatus`。我們必須知道 `ov->Internal` 填什麼值，才會讓 `DSWave.exe` 解讀為對應的 `libusb_transfer_status`。

#### 2. 反組譯追蹤步驟

**第一步：反組譯 `DSWave.exe` 的 `windows_handle_transfer_completion` (`0x140331ef8`)**
從字串 `"windows_handle_transfer_completion"` (`0x140dc8040`) 定位到 `0x140331ef8`：
```assembly
   140331f26: ff 15 e8 0a d6 00     call *0xd60ae8(%rip)      # 呼叫 IAT 0x141092a14 = KERNEL32!GetOverlappedResult
   140331f2c: 85 c0                 test %eax,%eax
   140331f2e: 74 75                 je   0x140331fa5          # 若回傳 FALSE (0)，跳到錯誤分類分支 0x140331fa5
   ...
   # 若回傳 TRUE (非 0)：直接當作 LIBUSB_TRANSFER_COMPLETED (0)！
   140331fa5: ff 15 41 0a d6 00     call *0xd60a41(%rip)      # %ebx = KERNEL32!GetLastError()
   ...
   140331fd8: 83 fb 79              cmp  $0x79,%ebx           # 0x79 (121) = ERROR_SEM_TIMEOUT -> LIBUSB_TRANSFER_TIMED_OUT (2)
   140331fe3: 81 fb e3 03 00 00     cmp  $0x3e3,%ebx          # 0x3e3 (995) = ERROR_OPERATION_ABORTED -> LIBUSB_TRANSFER_CANCELLED (3)
   140331fef: 81 fb 8f 04 00 00     cmp  $0x48f,%ebx          # 0x48f (1167) = ERROR_DEVICE_NOT_CONNECTED -> LIBUSB_TRANSFER_NO_DEVICE (5)
   14033206a: 83 fb 1f              cmp  $0x1f,%ebx           # 0x1f (31) = ERROR_GEN_FAILURE -> LIBUSB_TRANSFER_STALL (4)
   # 其他非 0 的 GetLastError() -> LIBUSB_TRANSFER_ERROR (1)
```

**第二步：反組譯 Wine 的 `kernelbase.dll!GetOverlappedResult` (`0x17402c840`)**
注意上面 `0x140331f26` 呼叫的是 **`KERNEL32!GetOverlappedResult`**，而不是 `WinUsb_GetOverlappedResult`！因此我們直接反組譯 Wine Prefix 中的 `kernelbase.dll`：
```bash
objdump -d --start-address=0x17402c840 --stop-address=0x17402c8e0 \
  "~/.wine-dswave/drive_c/windows/system32/kernelbase.dll"
```
看到關鍵指令：
```assembly
   17402c867: 8b 33                 mov  (%rbx),%esi          # %esi = (DWORD)ov->Internal
   ...
   17402c8a1: 48 8b 43 08           mov  0x8(%rbx),%rax       # %rax = ov->InternalHigh
   17402c8a5: 89 f1                 mov  %esi,%ecx            # 參數 1: (NTSTATUS)ov->Internal
   17402c8a7: 41 89 04 24           mov  %eax,(%r12)          # *lpNumberOfBytesTransferred = (DWORD)ov->InternalHigh
   17402c8ab: ff 15 6f fe 09 00     call *0x9fe6f(%rip)       # %eax = RtlNtStatusToDosError(ov->Internal)
   17402c8b3: 65 48 8b 04 25 30 ... mov  %gs:0x30,%rax        # 取得 TEB
   17402c8be: 89 50 68              mov  %edx,0x68(%rax)      # TEB->LastErrorValue = RtlNtStatusToDosError(ov->Internal)
   17402c8c1: 0f 94 c0              sete %al                  # 回傳 (ov->Internal == STATUS_SUCCESS || ov->Internal == STATUS_PENDING)
```

#### 3. 從反組譯推導出的對照表（供 [`winusb_shim.c`](../../wine-winusb-shim/src/winusb_shim.c) 使用）

由上述兩段反組譯，可建立 `helper.c` 回傳值 → `winusb_shim.c` (`ov->Internal`) → Wine `RtlNtStatusToDosError` → `DSWave.exe` 判定結果的完整精確映射表：

| `helper.c` 狀態 (`resp.status`) | `winusb_shim.c` 應填入 `ov->Internal` (`NTSTATUS`) | Wine `RtlNtStatusToDosError` 轉出的 Win32 Error | `DSWave.exe` (`0x140331ef8`) 轉換出的 `libusb_transfer_status` |
|---|---|---|---|
| `0` (成功) | `STATUS_SUCCESS` (`0x00000000`) | `0` (`ERROR_SUCCESS`) | `LIBUSB_TRANSFER_COMPLETED` (`0`) |
| `-7` (`LIBUSB_ERROR_TIMEOUT`) | `STATUS_IO_TIMEOUT` (`0xC00000B5`) | `0x79` (`121 = ERROR_SEM_TIMEOUT`) | `LIBUSB_TRANSFER_TIMED_OUT` (`2`) |
| `-10` (`LIBUSB_ERROR_INTERRUPTED`) | `STATUS_CANCELLED` (`0xC0000120`) | `0x3e3` (`995 = ERROR_OPERATION_ABORTED`) | `LIBUSB_TRANSFER_CANCELLED` (`3`) |
| `-9` (`LIBUSB_ERROR_PIPE`) | `STATUS_UNSUCCESSFUL` (`0xC0000001`) | `0x1f` (`31 = ERROR_GEN_FAILURE`) | `LIBUSB_TRANSFER_STALL` (`4`) |
| `-1` (`LIBUSB_ERROR_IO` / `-EPROTO`) | `STATUS_DEVICE_DATA_ERROR` (`0xC000009C`) | `0x17` (`23 = ERROR_CRC`) | `LIBUSB_TRANSFER_ERROR` (`1`) |
| `-4` (`LIBUSB_ERROR_NO_DEVICE`) | `STATUS_DEVICE_DOES_NOT_EXIST` (`0xC00000C0`) | `0x1b1` (`433 = ERROR_NO_SUCH_DEVICE`) | `LIBUSB_TRANSFER_NO_DEVICE` (`5`) |

---

## 四、實戰二：如何用反組譯確認 USB 通訊格式與硬體狀態機

當不知道 App 如何與 USB 硬體互動（有哪些封包階段、每個階段期待多少 bytes、何時算錯）時，按以下三步逆向：

### Step 1：傾印鄰近字串叢集（String Clustering）掌握協議全貌

編譯器會將同一個 `.cpp` 原始檔（例如 `DSWave.exe` 負責 USB 傳輸的模組）中的所有字串常數放在 `.rdata` 相鄰的位置。
先前我們找到 `"Data transfer failed:"` 位於檔案偏移 `0xb5a0d0`，只要把 `0xb59c00`～`0xb5a400` 之間的字串全部印出：

```bash
python3 -c '
data = open("~/.wine-dswave/drive_c/Program Files/DSWave/DSWave.exe", "rb").read()
for s in data[0xb59c00:0xb5a400].split(b"\x00"):
    if len(s) > 2: print(s.decode("ascii", errors="ignore"))
'
```
立刻就能看到 `DSWave.exe` 硬體抽象層（HAL）的完整協議階段名稱：
1. `SendStartPacket` / `%s: send HAL_PKT_TYPE_CTRL: START packet`
2. `HeadCallback` / `receive head packet: status %d; timeout: %d; length: %d`
3. `Head transfer size error: exp: %x; act: %x` / `Wrong head packet`
4. `receive data packet: status %s; timeout: %d; length: %d`
5. `Data transfer failed: %s; length: %d` / `Data transfer resubmitted successfully`
6. `Entering Drain Mode: Cancelling pending transfers to switch timeout...`
7. `%s: send HAL_PKT_TYPE_CTRL: END packet`

光看這組字串叢集，就能一目了然整個擷取生命週期分為：**START 控制封包 → HEAD 封包階段 → DATA 封包連續重送階段 → Drain Mode（取消殘留傳輸）→ END 控制封包**！

---

### Step 2：反組譯 `HeadCallback` (`0x1402b4a10`) 與 `DataCallback` (`0x1402b4df0`) 確認封包格式限制

這直接決定了 [`helper.c`](../../wine-winusb-shim/src/helper.c) 的 [`do_bulk()`](../../wine-winusb-shim/src/helper.c#L564-L833) 能不能切分封包（`chunk`）或合併封包（`fill`）：

#### 1. `HeadCallback` (`0x1402b4a10`) 的檢查邏輯
反組譯 `0x1402b4a6e`–`0x1402b4b97`：
```assembly
   1402b4a79: ff 90 e0 00 00 00     call *0xe0(%rax)          # 取得預期 Header 長度 (%rax = 4096)
   1402b4a7f: 80 7e 21 00           cmpb $0x0,0x21(%rsi)      # 檢查 session->stopping 是否為 1
   1402b4a83: 48 89 c5              mov  %rax,%rbp            # %ebp = 4096
   1402b4a86: 0f 85 cc 00 00 00     jne  0x1402b4b58          # 若正在停止，跳到 Transfer abort in HEAD phase
   1402b4a8c: 8b 53 10              mov  0x10(%rbx),%edx      # %edx = transfer->status
   1402b4a93: 85 d2                 test %edx,%edx
   1402b4a95: 75 71                 jne  0x1402b4b08          # 若 status != 0，印出 "Head transfer failed: %s"
   1402b4a97: 4c 63 43 18           movslq 0x18(%rbx),%r8     # %r8d = transfer->actual_length
   1402b4a9b: 41 39 e8              cmp  %ebp,%r8d            # 嚴格比對 actual_length == 4096！
   1402b4a9e: 0f 84 d4 00 00 00     je   0x1402b4b78          # 若相等，進入 0x1402b4b78 解析 Header 內容！
   # 若不相等，印出 "Head transfer size error: exp: %x; act: %x" (0x140b5b868)！
```
接著在 `0x1402b4b94` 呼叫解析器 `*0x18(%rax)`，驗證開頭 4 bytes 是否為同步字 `0x55555555`（若失敗則跳到 `0x1402b4bf0` 印出 `"Wrong head packet"`），成功後將狀態機 `0x30(%rsi)` 從 `1`（HEAD 階段）改為 `2`（DATA 階段，見 `0x1402b4bdb: movl $0x2, 0x30(%rsi)`）！

#### 2. `DataCallback` (`0x1402b4df0`) 的檢查邏輯
反組譯 `0x1402b4e51`–`0x1402b4ec7`：
```assembly
   1402b4e51: 80 7d 21 00           cmpb $0x0,0x21(%rbp)      # 檢查 session->stopping 是否為 1
   1402b4e55: 0f 85 04 01 00 00     jne  0x1402b4f5f          # 若正在停止，直接跳到 0x1402b4f5f 正常回收 transfer！
   1402b4e5b: 8b 43 10              mov  0x10(%rbx),%eax      # %eax = transfer->status
   1402b4e5e: 85 c0                 test %eax,%eax            # 檢查 transfer->status == 0
   1402b4e60: 0f 85 ca 00 00 00     jne  0x1402b4f30          # 若 status != 0 -> 呼叫 0x1402b4c90 ("Data transfer failed")
   1402b4e66: 4c 63 43 18           movslq 0x18(%rbx),%r8     # %r8d = transfer->actual_length
   1402b4e6a: 45 85 c0              test %r8d,%r8d            # 只檢查 actual_length != 0（不要求等於 buffer 長度！）
   1402b4e6d: 0f 84 bd 00 00 00     je   0x1402b4f30          # 若 actual_length == 0 -> 呼叫 0x1402b4c90 ("Data transfer failed")
```

#### 3. 從反組譯得出的 [`helper.c`](../../wine-winusb-shim/src/helper.c) 設計準則
1. **第一筆讀取（HEAD 階段，`req->len == 4096`）**：必須完整讀回 `4096` bytes，且開頭必須是 `55 55 55 55`；若上次殘留的幀資料沒排空導致讀回非 `55 55 55 55` 或長度非 `4096`，App 就會報 `Wrong head packet` 或 `Head transfer size error`。
2. **後續讀取（DATA 階段，`req->len == 5005312` 或 `405504`）**：App **只要求 `status == 0` 且 `actual_length > 0`**（但內部資料解碼器 `*0x20(%rax)` 以硬體封包對齊為單位處理），因此 `helper.c` 每次回傳一個完整的硬體 Burst 區塊（`405,504` bytes 的整數倍）完全符合 App 預期！
3. **為什麼絕不能回傳 `status = 0, len = 0`**：從 `0x1402b4e6a` (`test %r8d, %r8d; je 0x1402b4f30`) 可以清楚看到，在 `session->stopping == 0` 時，只要回傳 `actual_length == 0`（即使 `status == 0`），App 也會立刻跳進 `Data transfer failed` 並中止擷取！

---

### Step 3：對照反組譯與 USBPcap 確認硬體控制命令格式

當需要確認 Vendor Control Transfer（如 `bmRequestType = 0x40, bRequest = 0xb0 / 0xb1 / 0xb2`）的格式時：

1. **從反組譯找控制傳輸封裝函式**：
   搜尋呼叫 `libusb_control_transfer` 或 `libusb_submit_transfer` 且設定 `transfer->type = 0 (LIBUSB_TRANSFER_TYPE_CONTROL)` 的函式。
   例如在 `DSWave.exe` 中，控制命令以 3-byte payload (`[reg_addr, val_lo, val_hi]`) 發送到 `bRequest = 0xb0`。
2. **搭配 USBPcap CSV 驗證動態數值**：
   靜態反組譯負責確認「程式碼邏輯與分支條件」（例如 `0x1402b40b0` 在 Stop 時先設 `0x21(%rsi) = 1` 再取消 transfer），而動態錄包（如 [`/tmp/fpga1.csv`](file:///tmp/fpga1.csv#L2805-L2819)）則提供「執行期的實際暫存器數值順序」：
   - 啟動擷取：`03 01 00` → `02 00 00` → `01 01 00`
   - 停止擷取：`01 00 00` → `02 08 00` → 讀 `0x84` (1B) → 取消 `0x86` Bulk IN → `01 00 00` → `02 04 00`（重置 FPGA FIFO）

將靜態反組譯與動態封包結合，就能得知：**`01 00 00` 是硬體停止命令的第一步，此時 App 尚未完成 `0x84` 狀態讀取與設定 `session->stopping = 1`，若 `helper.c` 在收到第一筆 `01 00 00` 時就提早中斷 `0x86`，就會讓 `DataCallback` (`0x1402b4e51`) 因為看到 `session->stopping == 0` 而誤判為硬體異常斷線！**

---

## 五、日後排查問題的 5 步反組譯 SOP 速查表

未來若遇到任何未知的 App 報錯或卡頓，請依序執行以下 5 步：

1. **抓關鍵字串**：從終端機或 Log 複製錯誤訊息字串（例如 `"usb device is not ready"` 或 `"Head transfer failed"`）。
2. **算 `.rdata` VMA**：
   ```bash
   python3 -c '
   d = open("~/.wine-dswave/drive_c/Program Files/DSWave/DSWave.exe","rb").read()
   i = d.find(b"關鍵字串")
   print(hex(i), hex(i + 0x140001800))
   '
   ```
3. **找引用位址（Xref）**：
   ```bash
   objdump -d "~/.wine-dswave/drive_c/Program Files/DSWave/DSWave.exe" | grep "<VMA後8碼>" -B 25 -A 25
   ```
4. **反組譯整個函式並看跳轉條件（`cmp` / `test` + `je` / `jne`）**：
   ```bash
   objdump -d --start-address=0x140xxxxxx --stop-address=0x140yyyyyy \
     "~/.wine-dswave/drive_c/Program Files/DSWave/DSWave.exe"
   ```
   重點觀察跳入該錯誤區塊前的最後一個 `call` 與 `cmp`，確認它是檢查哪個 Win32 API 的回傳值、`GetLastError()`、還是封包長度。
5. **對照 Wine 系統 DLL（若涉及 Win32 API 轉換）**：
   若該 `call` 指向 `KERNEL32.dll` / `ntdll.dll`，用同樣方式反組譯 `~/.wine-dswave/drive_c/windows/system32/kernelbase.dll`，確認 Wine 內部如何檢查 `OVERLAPPED` 或 `HANDLE`，據此在 [`winusb_shim.c`](../../wine-winusb-shim/src/winusb_shim.c) 或 [`ioctl_hook.c`](../../wine-winusb-shim/src/ioctl_hook.c) 補上對應的狀態碼或 IAT Hook。
