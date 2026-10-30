/*
 * DeviceIoControl IAT hook + fake USB hub replies.
 *
 * libusb's Windows backend reads descriptors through the parent hub handle
 * (IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX /
 *  IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION), never through WinUSB.
 * Wine has no such device objects, so we intercept the calls made on our fake
 * ROOT_HUB file handle and answer them from data supplied by the helper.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include "shim_internal.h"

#pragma pack(push, 1)
struct conn_info_ex
{
    uint32_t ConnectionIndex;
    uint8_t DeviceDescriptor[18];
    uint8_t CurrentConfigurationValue;
    uint8_t Speed;
    uint8_t DeviceIsHub;
    uint16_t DeviceAddress;
    uint32_t NumberOfOpenPipes;
    uint32_t ConnectionStatus;
};

struct conn_info_v2
{
    uint32_t ConnectionIndex;
    uint32_t Length;
    uint32_t SupportedUsbProtocols; /* bit2 = Usb300 */
    uint32_t Flags;                 /* bit0 = operating at SS+, bit1 = SS capable */
};

struct descriptor_request
{
    uint32_t ConnectionIndex;
    uint8_t SetupPacket[8];
};
#pragma pack(pop)

#define IOCTL_USB_GET_NODE_INFORMATION              0x00220408
#define IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION 0x00220410
#define IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX  0x00220448
#define IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX_V2 0x0022045c

typedef BOOL (WINAPI *dioc_t)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD,
                              LPOVERLAPPED);
typedef HANDLE (WINAPI *ciocp_t)(HANDLE, HANDLE, ULONG_PTR, DWORD);
typedef BOOL (WINAPI *cancelioex_t)(HANDLE, LPOVERLAPPED);

static dioc_t real_dioc;
static ciocp_t real_ciocp;
static cancelioex_t real_cancelioex;
static int installed;

struct iocp_map
{
    HANDLE file;
    HANDLE iocp;
    ULONG_PTR key;
};
static struct iocp_map iocps[16];
static CRITICAL_SECTION iocp_lock;
static LONG iocp_lock_ready;

int shim_lookup_iocp(HANDLE file, HANDLE *iocp, ULONG_PTR *key)
{
    int i, found = 0;

    if (!iocp_lock_ready)
        return 0;
    EnterCriticalSection(&iocp_lock);
    for (i = 0; i < (int)ARRAYSIZE(iocps); i++)
    {
        if (iocps[i].file == file && file)
        {
            *iocp = iocps[i].iocp;
            *key = iocps[i].key;
            found = 1;
            break;
        }
    }
    LeaveCriticalSection(&iocp_lock);
    return found;
}

void shim_forget_iocp(HANDLE file)
{
    int i;

    if (!iocp_lock_ready)
        return;
    EnterCriticalSection(&iocp_lock);
    for (i = 0; i < (int)ARRAYSIZE(iocps); i++)
        if (iocps[i].file == file)
        {
            iocps[i].file = NULL;
            iocps[i].iocp = NULL;
            iocps[i].key = 0;
        }
    LeaveCriticalSection(&iocp_lock);
}

static HANDLE WINAPI hook_CreateIoCompletionPort(HANDLE file, HANDLE existing,
                                                 ULONG_PTR key, DWORD threads)
{
    HANDLE ret = real_ciocp(file, existing, key, threads);

    if (ret && existing)
    {
        int i;
        EnterCriticalSection(&iocp_lock);
        for (i = 0; i < (int)ARRAYSIZE(iocps); i++)
        {
            if (iocps[i].file == file || !iocps[i].file)
            {
                iocps[i].file = file;
                iocps[i].iocp = ret;
                iocps[i].key = key;
                break;
            }
        }
        LeaveCriticalSection(&iocp_lock);
    }
    return ret;
}

static int hub_handle(HANDLE h)
{
    WCHAR path[MAX_PATH];

    if (!h || h == INVALID_HANDLE_VALUE)
        return 0;
    if (!GetFinalPathNameByHandleW(h, path, ARRAYSIZE(path), FILE_NAME_NORMALIZED))
        return 0;
    return wcsstr(path, L"ROOT_HUB") != NULL;
}

static const struct dev_info *dev_info(void)
{
    static struct dev_info info;
    static int loaded;

    if (!loaded && shim_get_dev_info(&info) == 0)
        loaded = 1;
    return loaded ? &info : NULL;
}

static BOOL complete(LPDWORD ret, LPOVERLAPPED ov, DWORD bytes)
{
    if (ret)
        *ret = bytes;
    if (ov)
    {
        ov->Internal = 0;
        ov->InternalHigh = bytes;
        if (ov->hEvent)
            SetEvent(ov->hEvent);
    }
    return TRUE;
}

static BOOL hub_ioctl(HANDLE h, DWORD code, LPVOID in, DWORD in_len, LPVOID out,
                      DWORD out_len, LPDWORD ret, LPOVERLAPPED ov)
{
    const struct dev_info *info = dev_info();
    DWORD idx = 0;

    if (!info || !out || !out_len)
        return FALSE;

    if (in && in_len >= 4)
        idx = *(const uint32_t *)in;

    switch (code)
    {
        case IOCTL_USB_GET_NODE_INFORMATION:
        {
            uint8_t *p = out;
            DWORD n = out_len < 32 ? out_len : 32;

            memset(p, 0, out_len);
            if (n >= 7)
            {
                p[0] = 0;    /* NodeType = UsbHub */
                p[4] = 9;    /* bDescriptorLength */
                p[5] = 0x29; /* hub descriptor type */
                p[6] = 8;    /* bNumberOfPorts */
            }
            return complete(ret, ov, out_len);
        }
        case IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX:
        {
            struct conn_info_ex *ci;
            DWORD n;

            if (out_len < sizeof(*ci))
                return FALSE;
            ci = out;
            memset(ci, 0, sizeof(*ci));
            ci->ConnectionIndex = idx;
            memcpy(ci->DeviceDescriptor, info->device_desc, sizeof(ci->DeviceDescriptor));
            ci->CurrentConfigurationValue = 1;
            ci->Speed = info->speed >= 3 ? 3 : (uint8_t)info->speed;
            ci->DeviceIsHub = 0;
            ci->DeviceAddress = (uint16_t)info->addr;
            ci->NumberOfOpenPipes = 3;
            ci->ConnectionStatus = (idx == info->addr) ? 1 : 0;
            n = out_len;
            return complete(ret, ov, n);
        }
        case IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX_V2:
        {
            struct conn_info_v2 *v2;

            if (out_len < sizeof(*v2))
                return FALSE;
            v2 = out;
            v2->ConnectionIndex = idx;
            v2->Length = sizeof(*v2);
            v2->SupportedUsbProtocols = 0x04; /* Usb300 */
            v2->Flags = (idx == info->addr) ? 0x03 : 0x00;
            return complete(ret, ov, sizeof(*v2));
        }
        case IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION:
        {
            const struct descriptor_request *req = in;
            struct descriptor_request *rsp = out;
            DWORD header = sizeof(*req);
            DWORD copy = 0;
            uint16_t wvalue = 0;

            if (!in || in_len < header || out_len <= header)
                return FALSE;
            memcpy(rsp, req, header);
            wvalue = (uint16_t)(req->SetupPacket[2] | (req->SetupPacket[3] << 8));
            if ((wvalue >> 8) == 0x01)
            {
                copy = out_len - header;
                if (copy > SHIM_DEV_DESC_LEN)
                    copy = SHIM_DEV_DESC_LEN;
                memcpy((uint8_t *)rsp + header, info->device_desc, copy);
            }
            else
            {
                copy = out_len - header;
                if (copy > info->config_len)
                    copy = info->config_len;
                memcpy((uint8_t *)rsp + header, info->config_desc, copy);
            }
            return complete(ret, ov, header + copy);
        }
        default:
            break;
    }
    (void)h;
    return FALSE;
}

static BOOL WINAPI hook_DeviceIoControl(HANDLE h, DWORD code, LPVOID in, DWORD in_len,
                                        LPVOID out, DWORD out_len, LPDWORD ret,
                                        LPOVERLAPPED ov)
{
    if (hub_handle(h) && hub_ioctl(h, code, in, in_len, out, out_len, ret, ov))
        return TRUE;
    return real_dioc(h, code, in, in_len, out, out_len, ret, ov);
}

/* libusb's windows_cancel_transfer() calls CancelIoEx() first and treats
 * ERROR_NOT_FOUND as "transfer already finished", skipping WinUsb_AbortPipe().
 * Our device handle is a plain file with no kernel I/O, so CancelIoEx would
 * always fail that way and the app would wait forever for a completion.
 * Instead, ask winusb.dll to complete the overlapped itself as cancelled. */
typedef BOOL (WINAPI *cancel_ov_t)(LPOVERLAPPED);
static cancel_ov_t cancel_ov_fn;
static CRITICAL_SECTION cancel_log_lock;
static LONG cancel_log_state; /* 0=uninit 1=initializing 2=ready */

static void cancel_log_init(void)
{
    for (;;)
    {
        LONG s = InterlockedCompareExchange(&cancel_log_state, 1, 0);
        if (s == 0)
        {
            InitializeCriticalSection(&cancel_log_lock);
            InterlockedExchange(&cancel_log_state, 2);
            return;
        }
        if (s == 2)
            return;
        Sleep(1);
    }
}

static void cancel_log(const char *fmt, ...)
{
    char buf[512];
    va_list args;
    FILE *f;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    cancel_log_init();
    EnterCriticalSection(&cancel_log_lock);
    f = fopen("C:\\ProgramData\\DSWaveShim\\cancelio.log", "a");
    if (f)
    {
        fprintf(f, "%s\n", buf);
        fclose(f);
    }
    LeaveCriticalSection(&cancel_log_lock);
}

static cancel_ov_t get_cancel_ov(void)
{
    HMODULE m;

    if (cancel_ov_fn)
        return cancel_ov_fn;
    m = GetModuleHandleA("winusb.dll");
    if (!m)
        m = LoadLibraryA("winusb.dll");
    if (m)
        cancel_ov_fn = (cancel_ov_t)(void *)GetProcAddress(m, "shim_cancel_overlapped");
    return cancel_ov_fn;
}

static BOOL WINAPI hook_CancelIoEx(HANDLE h, LPOVERLAPPED ov)
{
    const char *off;

    off = getenv("DSW_CANCELIO_HOOK");
    if (off && *off == '0')
        return real_cancelioex ? real_cancelioex(h, ov) : FALSE;

    /* Fast path: shim_cancel_overlapped() only matches OVERLAPPEDs that are
     * actually pending in our bulk queue, so we can try it for every handle -
     * no need to resolve the file path first (that check silently failed and
     * made the hook a no-op). */
    if (ov)
    {
        cancel_ov_t fn = get_cancel_ov();

        if (fn && fn(ov))
        {
            cancel_log("CancelIoEx h=%p ov=%p -> matched pending, cancelled", (void *)h, (void *)ov);
            SetLastError(ERROR_SUCCESS);
            return TRUE;
        }
    }
    {
        WCHAR path[MAX_PATH];

        if (h && h != INVALID_HANDLE_VALUE &&
            GetFinalPathNameByHandleW(h, path, ARRAYSIZE(path), FILE_NAME_NORMALIZED) &&
            wcsstr(path, L"DSWaveShim"))
        {
            cancel_log("CancelIoEx h=%p ov=%p -> shim handle, ov not pending", (void *)h, (void *)ov);
            SetLastError(ERROR_NOT_FOUND);
            return FALSE;
        }
    }
    cancel_log("CancelIoEx h=%p ov=%p -> passthrough", (void *)h, (void *)ov);
    return real_cancelioex ? real_cancelioex(h, ov) : FALSE;
}

void ioctl_hook_install(void)
{
    HMODULE exe;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    DWORD i;
    int n_dioc = 0, n_cancel = 0, n_ciocp = 0;

    if (installed)
        return;
    exe = GetModuleHandleW(NULL);
    if (!exe)
        return;
    dos = (IMAGE_DOS_HEADER *)exe;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    nt = (IMAGE_NT_HEADERS *)((BYTE *)exe + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return;
    imp = (IMAGE_IMPORT_DESCRIPTOR *)((BYTE *)exe +
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

    for (; imp->Name; imp++)
    {
        IMAGE_THUNK_DATA *first;
        IMAGE_THUNK_DATA *names;
        const char *dll = (const char *)exe + imp->Name;

        if (_stricmp(dll, "KERNEL32.dll") && _stricmp(dll, "kernel32.dll"))
            continue;
        first = (IMAGE_THUNK_DATA *)((BYTE *)exe + imp->FirstThunk);
        names = imp->OriginalFirstThunk
                    ? (IMAGE_THUNK_DATA *)((BYTE *)exe + imp->OriginalFirstThunk)
                    : NULL;
        if (!names)
            return;

        for (i = 0; first[i].u1.Function; i++)
        {
            IMAGE_IMPORT_BY_NAME *name;
            DWORD old;

            if (IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal))
                continue;
            name = (IMAGE_IMPORT_BY_NAME *)((BYTE *)exe + names[i].u1.AddressOfData);
            if (!strcmp((const char *)name->Name, "DeviceIoControl"))
            {
                if (!VirtualProtect(&first[i].u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &old))
                    return;
                real_dioc = (dioc_t)first[i].u1.Function;
                first[i].u1.Function = (ULONG_PTR)hook_DeviceIoControl;
                VirtualProtect(&first[i].u1.Function, sizeof(ULONG_PTR), old, &old);
                installed = 1;
                n_dioc++;
            }
            else if (!strcmp((const char *)name->Name, "CancelIoEx"))
            {
                if (!VirtualProtect(&first[i].u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &old))
                    return;
                real_cancelioex = (cancelioex_t)first[i].u1.Function;
                first[i].u1.Function = (ULONG_PTR)hook_CancelIoEx;
                VirtualProtect(&first[i].u1.Function, sizeof(ULONG_PTR), old, &old);
                n_cancel++;
            }
            else if (!strcmp((const char *)name->Name, "CreateIoCompletionPort"))
            {
                if (!VirtualProtect(&first[i].u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &old))
                    return;
                real_ciocp = (ciocp_t)first[i].u1.Function;
                first[i].u1.Function = (ULONG_PTR)hook_CreateIoCompletionPort;
                VirtualProtect(&first[i].u1.Function, sizeof(ULONG_PTR), old, &old);
                n_ciocp++;
                if (!iocp_lock_ready)
                {
                    InitializeCriticalSection(&iocp_lock);
                    InterlockedExchange(&iocp_lock_ready, 1);
                }
            }
        }
    }
    cancel_log("install: dioc=%d cancelio=%d ciocp=%d", n_dioc, n_cancel, n_ciocp);
}
