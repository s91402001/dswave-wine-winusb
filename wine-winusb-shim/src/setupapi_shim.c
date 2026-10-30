/*
 * setupapi.dll shim for DSWave under Wine.
 *
 * Wine has no USB function driver, so libusb's Windows backend can never build
 * a device tree.  We synthesise the minimal PnP tree libusb needs:
 *
 *   HCD 0x3000  (GUID_DEVINTERFACE_USB_HOST_CONTROLLER)
 *    └ HUB 0x2000 (GUID_DEVINTERFACE_USB_HUB, "USB\ROOT_HUB30\...")
 *       └ DEV 0x1000 (GUID_DEVINTERFACE_USB_DEVICE, the real 2a0e:0080)
 *
 * Every device points at a real, openable file so that libusb's CreateFileA()
 * succeeds; the DeviceIoControl replies are produced by ioctl_hook.c.
 * Everything else is forwarded to setupapi_orig.dll.
 */
#define WIN32_LEAN_AND_MEAN
#include <initguid.h>
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "proto.h"
#include "shim_ids.h"
#include "shim_internal.h"

DEFINE_GUID(SHIM_GUID_WINUSB, 0xdee824ef, 0x729b, 0x4a0e, 0x9c, 0x14,
            0xb7, 0x11, 0x7d, 0x33, 0xa8, 0x17);
DEFINE_GUID(SHIM_GUID_USB_DEVICE, 0xa5dcbf10, 0x6530, 0x11d2, 0x90, 0x1f,
            0x00, 0xc0, 0x4f, 0xb9, 0x51, 0xed);
DEFINE_GUID(SHIM_GUID_USB_CLASS, 0x36fc9e60, 0xc465, 0x11cf, 0x80, 0x56, 0x44,
            0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(SHIM_GUID_USB_HUB, 0xf18a0e88, 0xc30c, 0x11d0, 0x88, 0x15,
            0x00, 0xa0, 0xc9, 0x06, 0xbe, 0xd8);
DEFINE_GUID(SHIM_GUID_USB_HCD, 0x3abf6f2d, 0x71c4, 0x462a, 0x8a, 0x92,
            0x1e, 0x68, 0x61, 0xe6, 0xaf, 0x27);

#define FAKE_MAGIC 0x44535732 /* "DSW2" */
#define MAX_SETS 16

struct fake_dev
{
    DWORD devinst;
    WCHAR instance_id[MAX_PATH];
    const WCHAR *desc;
    const WCHAR *enumerator;
    WCHAR path[MAX_PATH];
    uint32_t bus, addr;
    int parent;                 /* 0 = none */
    int is_root_hub;
    int n_ifaces;
    GUID ifaces[2];
};

struct fake_set
{
    DWORD magic;
    int kind;                   /* 0 = interface set, 1 = enumerator set */
    int n;
    const struct fake_dev *devs[3];
};

static struct fake_dev devices[3];
static int devices_ready;
static struct fake_set *sets[MAX_SETS];
static HMODULE orig_module;
static CRITICAL_SECTION lock;
static LONG initialized;

static const WCHAR CLASS_GUID_STR[] = L"{36FC9E60-C465-11CF-8056-444553540000}";

/* ------------------------------------------------------------------ */

static void shim_log(const char *fmt, ...)
{
    char buf[512];
    va_list args;
    FILE *f;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    fprintf(stderr, "SHIM[%lu]: %s\n", (unsigned long)GetCurrentProcessId(), buf);
    f = fopen("C:\\ProgramData\\DSWaveShim\\setupapi.log", "a");
    if (f)
    {
        fprintf(f, "[%lu] %s\n", (unsigned long)GetCurrentProcessId(), buf);
        fclose(f);
    }
}

static FARPROC orig(const char *name)
{
    if (!orig_module)
        orig_module = LoadLibraryA("C:\\windows\\system32\\setupapi_orig.dll");
    if (!orig_module)
        return NULL;
    return GetProcAddress(orig_module, name);
}

static int guid_is(const GUID *a, const GUID *b)
{
    return a->Data1 == b->Data1 && a->Data2 == b->Data2 && a->Data3 == b->Data3 &&
           !memcmp(a->Data4, b->Data4, sizeof(a->Data4));
}

static int is_usb_guid(const GUID *guid)
{
    return guid && (guid_is(guid, &SHIM_GUID_WINUSB) ||
                    guid_is(guid, &SHIM_GUID_USB_DEVICE) ||
                    guid_is(guid, &SHIM_GUID_USB_CLASS) ||
                    guid_is(guid, &SHIM_GUID_USB_HUB) ||
                    guid_is(guid, &SHIM_GUID_USB_HCD));
}

static void ensure_file(const WCHAR *path)
{
    HANDLE h;

    CreateDirectoryW(L"C:\\ProgramData\\DSWaveShim", NULL);
    h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
}

/* The device can re-enumerate (new bus address) when it resets after a
 * capture, so the synthetic node identity is refreshed on enumeration.  The
 * IPC call is throttled: the app's hotplug thread enumerates every ~100 ms,
 * and each unthrottled call used to open the device and hit EP0, which froze
 * the FX3 in the middle of a bulk stream (progress.md 2026-10-30). */
static void refresh_dev_identity(void)
{
    struct dev_info info;
    uint32_t bus = devices[2].bus ? devices[2].bus : 8;
    uint32_t addr = devices[2].addr ? devices[2].addr : 6;
    static DWORD last_tick;
    static int cached;
    static const char *live;

    if (!live)
    {
        const char *e = getenv("DSW_LIST_LIVE");
        live = (e && *e && *e != '0') ? e : NULL;
    }
    if (!live && cached && GetTickCount() - last_tick < 2000)
        return;

    if (shim_get_dev_info(&info) == 0)
    {
        bus = info.bus;
        addr = info.addr;
    }
    last_tick = GetTickCount();
    cached = 1;
    devices[2].bus = bus;
    devices[2].addr = addr;
    swprintf(devices[2].instance_id, MAX_PATH, L"USB\\VID_2A0E&PID_0080\\%u&%u", bus, addr);
    swprintf(devices[2].path, MAX_PATH,
             L"\\\\?\\C:\\ProgramData\\DSWaveShim\\USB#VID_2A0E&PID_0080#%u&%u"
             L"#{A5DCBF10-6530-11D2-901F-00C04FB951ED}", bus, addr);
    ensure_file(devices[2].path);
    shim_log("device identity: bus=%u addr=%u", bus, addr);
}

static void init_devices(void)
{
    if (devices_ready)
        return;

    memset(devices, 0, sizeof(devices));

    devices[0].devinst = SHIM_DEVINST_HCD;
    wcscpy(devices[0].instance_id, L"PCI\\VEN_8086&DEV_1E31&SUBSYS_00000000&REV_00\\0");
    devices[0].desc = L"USB xHCI Compliant Host Controller";
    devices[0].enumerator = L"PCI";
    devices[0].parent = 0;
    devices[0].n_ifaces = 1;
    devices[0].ifaces[0] = SHIM_GUID_USB_HCD;
    wcscpy(devices[0].path, L"\\\\?\\C:\\ProgramData\\DSWaveShim\\HCD.dev");

    devices[1].devinst = SHIM_DEVINST_HUB;
    wcscpy(devices[1].instance_id, L"USB\\ROOT_HUB30\\4&0&0");
    devices[1].desc = L"USB Root Hub (USB 3.0)";
    devices[1].enumerator = L"USB";
    devices[1].parent = SHIM_DEVINST_HCD;
    devices[1].is_root_hub = 1;
    devices[1].n_ifaces = 1;
    devices[1].ifaces[0] = SHIM_GUID_USB_HUB;
    wcscpy(devices[1].path, L"\\\\?\\C:\\ProgramData\\DSWaveShim\\ROOT_HUB");

    devices[2].devinst = SHIM_DEVINST_USB;
    devices[2].bus = 8;
    devices[2].addr = 6;
    devices[2].desc = L"USB-based DSL Instrument v3";
    devices[2].enumerator = L"USB";
    devices[2].parent = SHIM_DEVINST_HUB;
    devices[2].n_ifaces = 2;
    devices[2].ifaces[0] = SHIM_GUID_USB_DEVICE;
    devices[2].ifaces[1] = SHIM_GUID_WINUSB;
    refresh_dev_identity();

    ensure_file(devices[0].path);
    ensure_file(devices[1].path);
    ensure_file(devices[2].path);
    devices_ready = 1;
}

const WCHAR *shim_hub_path(void)
{
    init_devices();
    return devices[1].path;
}

/* ------------------------------------------------------------------ */

static int recv_exact(SOCKET s, void *buf, size_t len)
{
    char *p = buf;
    while (len)
    {
        int n = recv(s, p, (int)len, 0);
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int shim_get_dev_info(struct dev_info *out)
{
    SOCKET s;
    struct sockaddr_in addr;
    struct shim_req req;
    struct shim_resp resp;
    int ret = -1;

    memset(out, 0, sizeof(*out));
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET)
        return -1;
    {
        DWORD tv = 3000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SHIM_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        goto out;
    memset(&req, 0, sizeof(req));
    req.type = REQ_LIST;
    if (send(s, (const char *)&req, sizeof(req), 0) != sizeof(req))
        goto out;
    if (recv_exact(s, &resp, sizeof(resp)))
        goto out;
    if (resp.status || !resp.len)
        goto out;
    if (recv_exact(s, out, sizeof(*out)))
        goto out;
    ret = 0;
out:
    closesocket(s);
    return ret;
}

/* ------------------------------------------------------------------ */

static struct fake_set *set_from(HDEVINFO h)
{
    struct fake_set *f = (struct fake_set *)h;
    int i;

    for (i = 0; i < MAX_SETS; i++)
        if (sets[i] == f)
            return f;
    return NULL;
}

static struct fake_set *make_set(int kind)
{
    struct fake_set *set;
    int i, slot;

    init_devices();
    refresh_dev_identity();
    set = calloc(1, sizeof(*set));
    if (!set)
        return NULL;
    set->magic = FAKE_MAGIC;
    set->kind = kind;
    if (kind == 0)
    {
        set->devs[0] = &devices[0];
        set->devs[1] = &devices[1];
        set->devs[2] = &devices[2];
        set->n = 3;
    }
    else
    {
        set->devs[0] = &devices[1];
        set->devs[1] = &devices[2];
        set->n = 2;
    }
    for (slot = 0; slot < MAX_SETS; slot++)
        if (!sets[slot])
            break;
    if (slot == MAX_SETS)
    {
        free(set);
        return NULL;
    }
    sets[slot] = set;
    for (i = 0; i < set->n; i++)
        shim_log("set kind=%d dev=%#lx", kind, (unsigned long)set->devs[i]->devinst);
    return set;
}

static int wants_fake(const GUID *guid, const WCHAR *enumerator, DWORD flags)
{
    if (is_usb_guid(guid))
        return 1;
    if (enumerator && !_wcsicmp(enumerator, L"USB"))
        return 1;
    if (!guid && (flags & DIGCF_ALLCLASSES) && (flags & DIGCF_DEVICEINTERFACE))
        return 1;
    return 0;
}

static int fake_kind(const GUID *guid, const WCHAR *enumerator, DWORD flags)
{
    (void)enumerator;
    if (!guid && (flags & DIGCF_DEVICEINTERFACE))
        return 0;               /* interface set: HCD + HUB + DEV */
    if (!guid)
        return 1;               /* "USB" enumerator set: HUB + DEV */
    return 0;                   /* explicit GUID: device interfaces only */
}

static struct fake_dev *dev_by_inst(DWORD devinst)
{
    init_devices();
    if (devinst == SHIM_DEVINST_HCD)
        return &devices[0];
    if (devinst == SHIM_DEVINST_HUB)
        return &devices[1];
    if (devinst == SHIM_DEVINST_USB)
        return &devices[2];
    return NULL;
}

static int dev_has_iface(const struct fake_dev *dev, const GUID *guid)
{
    int i;

    if (!guid)
        return 0;
    for (i = 0; i < dev->n_ifaces; i++)
        if (guid_is(guid, &dev->ifaces[i]))
            return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* class devs */

HDEVINFO WINAPI SetupDiGetClassDevsW(const GUID *guid, PCWSTR enumerator, HWND parent,
                                     DWORD flags)
{
    typedef HDEVINFO (WINAPI *fn_t)(const GUID *, PCWSTR, HWND, DWORD);

    shim_log("GetClassDevsW guid=%08lx enum=%ls flags=%lx",
             (unsigned long)(guid ? guid->Data1 : 0), enumerator ? enumerator : L"(null)",
             (unsigned long)flags);
    if (wants_fake(guid, enumerator, flags))
        return (HDEVINFO)make_set(fake_kind(guid, enumerator, flags));
    return ((fn_t)orig("SetupDiGetClassDevsW"))(guid, enumerator, parent, flags);
}

HDEVINFO WINAPI SetupDiGetClassDevsA(const GUID *guid, PCSTR enumerator, HWND parent,
                                     DWORD flags)
{
    typedef HDEVINFO (WINAPI *fn_t)(const GUID *, PCSTR, HWND, DWORD);
    WCHAR wide[64];

    shim_log("GetClassDevsA guid=%08lx enum=%s flags=%lx",
             (unsigned long)(guid ? guid->Data1 : 0), enumerator ? enumerator : "(null)",
             (unsigned long)flags);
    if (enumerator)
        MultiByteToWideChar(CP_ACP, 0, enumerator, -1, wide, ARRAYSIZE(wide));
    if (wants_fake(guid, enumerator ? wide : NULL, flags))
        return (HDEVINFO)make_set(fake_kind(guid, enumerator ? wide : NULL, flags));
    return ((fn_t)orig("SetupDiGetClassDevsA"))(guid, enumerator, parent, flags);
}

HDEVINFO WINAPI SetupDiGetClassDevsExW(const GUID *guid, PCWSTR enumerator, HWND parent,
                                       DWORD flags, HDEVINFO set, PCWSTR machine,
                                       PVOID reserved)
{
    typedef HDEVINFO (WINAPI *fn_t)(const GUID *, PCWSTR, HWND, DWORD, HDEVINFO, PCWSTR, PVOID);

    shim_log("GetClassDevsExW guid=%08lx enum=%ls flags=%lx",
             (unsigned long)(guid ? guid->Data1 : 0), enumerator ? enumerator : L"(null)",
             (unsigned long)flags);
    if (wants_fake(guid, enumerator, flags))
        return (HDEVINFO)make_set(fake_kind(guid, enumerator, flags));
    return ((fn_t)orig("SetupDiGetClassDevsExW"))(guid, enumerator, parent, flags, set,
                                                  machine, reserved);
}

HDEVINFO WINAPI SetupDiGetClassDevsExA(const GUID *guid, PCSTR enumerator, HWND parent,
                                       DWORD flags, HDEVINFO set, PCSTR machine,
                                       PVOID reserved)
{
    typedef HDEVINFO (WINAPI *fn_t)(const GUID *, PCSTR, HWND, DWORD, HDEVINFO, PCSTR, PVOID);
    WCHAR wide[64];

    if (enumerator)
        MultiByteToWideChar(CP_ACP, 0, enumerator, -1, wide, ARRAYSIZE(wide));
    if (wants_fake(guid, enumerator ? wide : NULL, flags))
        return (HDEVINFO)make_set(fake_kind(guid, enumerator ? wide : NULL, flags));
    return ((fn_t)orig("SetupDiGetClassDevsExA"))(guid, enumerator, parent, flags, set,
                                                  machine, reserved);
}

BOOL WINAPI SetupDiDestroyDeviceInfoList(HDEVINFO h)
{
    typedef BOOL (WINAPI *fn_t)(HDEVINFO);
    struct fake_set *f = set_from(h);
    int i;

    if (f)
    {
        for (i = 0; i < MAX_SETS; i++)
            if (sets[i] == f)
                sets[i] = NULL;
        f->magic = 0;
        free(f);
        return TRUE;
    }
    return ((fn_t)orig("SetupDiDestroyDeviceInfoList"))(h);
}

HDEVINFO WINAPI SetupDiCreateDeviceInfoList(const GUID *class_guid, HWND parent)
{
    typedef HDEVINFO (WINAPI *fn_t)(const GUID *, HWND);
    return ((fn_t)orig("SetupDiCreateDeviceInfoList"))(class_guid, parent);
}

/* ------------------------------------------------------------------ */

BOOL WINAPI SetupDiEnumDeviceInfo(HDEVINFO h, DWORD index, PSP_DEVINFO_DATA data)
{
    typedef BOOL (WINAPI *fn_t)(HDEVINFO, DWORD, PSP_DEVINFO_DATA);
    struct fake_set *f = set_from(h);

    if (f)
    {
        if (index >= (DWORD)f->n)
        {
            SetLastError(ERROR_NO_MORE_ITEMS);
            return FALSE;
        }
        if (!data || data->cbSize != sizeof(*data))
        {
            SetLastError(ERROR_INVALID_USER_BUFFER);
            return FALSE;
        }
        data->ClassGuid = SHIM_GUID_USB_CLASS;
        data->DevInst = f->devs[index]->devinst;
        data->Reserved = 0;
        return TRUE;
    }
    return ((fn_t)orig("SetupDiEnumDeviceInfo"))(h, index, data);
}

BOOL WINAPI SetupDiEnumDeviceInterfaces(HDEVINFO h, PSP_DEVINFO_DATA data,
                                        const GUID *guid, DWORD index,
                                        PSP_DEVICE_INTERFACE_DATA iface)
{
    typedef BOOL (WINAPI *fn_t)(HDEVINFO, PSP_DEVINFO_DATA, const GUID *, DWORD,
                                PSP_DEVICE_INTERFACE_DATA);
    struct fake_set *f = set_from(h);
    struct fake_dev *match = NULL;

    if (!iface || iface->cbSize != sizeof(*iface))
    {
        SetLastError(ERROR_INVALID_USER_BUFFER);
        return FALSE;
    }

    if (f)
    {
        if (data)
        {
            struct fake_dev *dev = dev_by_inst(data->DevInst);
            if (dev && dev_has_iface(dev, guid) && index == 0)
                match = dev;
        }
        else
        {
            DWORD seen = 0;
            int i;
            for (i = 0; i < f->n; i++)
            {
                if (!dev_has_iface(f->devs[i], guid))
                    continue;
                if (seen++ == index)
                {
                    match = (struct fake_dev *)f->devs[i];
                    break;
                }
            }
        }
        if (!match)
        {
            SetLastError(ERROR_NO_MORE_ITEMS);
            return FALSE;
        }
        iface->InterfaceClassGuid = *guid;
        iface->Flags = SPINT_ACTIVE;
        iface->Reserved = (ULONG_PTR)match->devinst;
        shim_log("EnumDeviceInterfaces -> dev=%#lx guid=%08lx",
                 (unsigned long)match->devinst, (unsigned long)guid->Data1);
        return TRUE;
    }
    return ((fn_t)orig("SetupDiEnumDeviceInterfaces"))(h, data, guid, index, iface);
}

static BOOL detail_w(struct fake_dev *dev, PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail,
                     DWORD size, PDWORD required, PSP_DEVINFO_DATA info)
{
    DWORD need = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) +
                 (DWORD)((wcslen(dev->path) + 1) * sizeof(WCHAR));

    if (required)
        *required = need;
    if (!detail || size < need)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    wcscpy(detail->DevicePath, dev->path);
    if (info)
    {
        info->ClassGuid = SHIM_GUID_USB_CLASS;
        info->DevInst = dev->devinst;
        info->Reserved = 0;
    }
    return TRUE;
}

BOOL WINAPI SetupDiGetDeviceInterfaceDetailW(HDEVINFO h, PSP_DEVICE_INTERFACE_DATA iface,
                                             PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail,
                                             DWORD size, PDWORD required,
                                             PSP_DEVINFO_DATA info)
{
    typedef BOOL (WINAPI *fn_t)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA,
                                PSP_DEVICE_INTERFACE_DETAIL_DATA_W, DWORD, PDWORD,
                                PSP_DEVINFO_DATA);
    struct fake_set *f = set_from(h);
    struct fake_dev *dev;

    if (f)
    {
        dev = dev_by_inst((DWORD)(iface ? iface->Reserved : 0));
        if (!dev)
            dev = &devices[2];
        return detail_w(dev, detail, size, required, info);
    }
    return ((fn_t)orig("SetupDiGetDeviceInterfaceDetailW"))(h, iface, detail, size,
                                                            required, info);
}

BOOL WINAPI SetupDiGetDeviceInterfaceDetailA(HDEVINFO h, PSP_DEVICE_INTERFACE_DATA iface,
                                             PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail,
                                             DWORD size, PDWORD required,
                                             PSP_DEVINFO_DATA info)
{
    typedef BOOL (WINAPI *fn_t)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA,
                                PSP_DEVICE_INTERFACE_DETAIL_DATA_A, DWORD, PDWORD,
                                PSP_DEVINFO_DATA);
    struct fake_set *f = set_from(h);
    struct fake_dev *dev;

    if (f)
    {
        DWORD need;
        char narrow[MAX_PATH * 2];

        dev = dev_by_inst((DWORD)(iface ? iface->Reserved : 0));
        if (!dev)
            dev = &devices[2];
        need = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A) +
               (DWORD)((wcslen(dev->path) + 1) * sizeof(WCHAR));
        if (required)
            *required = need;
        if (!detail || size < need)
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        WideCharToMultiByte(CP_ACP, 0, dev->path, -1, narrow, sizeof(narrow), NULL, NULL);
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        strcpy(detail->DevicePath, narrow);
        if (info)
        {
            info->ClassGuid = SHIM_GUID_USB_CLASS;
            info->DevInst = dev->devinst;
            info->Reserved = 0;
        }
        return TRUE;
    }
    return ((fn_t)orig("SetupDiGetDeviceInterfaceDetailA"))(h, iface, detail, size,
                                                            required, info);
}

BOOL WINAPI SetupDiOpenDeviceInterfaceW(HDEVINFO h, PCWSTR name, DWORD flags,
                                        PSP_DEVICE_INTERFACE_DATA iface)
{
    typedef BOOL (WINAPI *fn_t)(HDEVINFO, PCWSTR, DWORD, PSP_DEVICE_INTERFACE_DATA);
    struct fake_set *f = set_from(h);
    int i;

    if (f)
    {
        for (i = 0; i < 3; i++)
        {
            if (!_wcsicmp(name, devices[i].path))
            {
                if (iface)
                {
                    iface->cbSize = sizeof(*iface);
                    iface->InterfaceClassGuid = devices[i].ifaces[0];
                    iface->Flags = SPINT_ACTIVE | (flags & SPINT_DEFAULT);
                    iface->Reserved = devices[i].devinst;
                }
                return TRUE;
            }
        }
        SetLastError(ERROR_INVALID_NAME);
        return FALSE;
    }
    return ((fn_t)orig("SetupDiOpenDeviceInterfaceW"))(h, name, flags, iface);
}

BOOL WINAPI SetupDiOpenDeviceInterfaceA(HDEVINFO h, PCSTR name, DWORD flags,
                                        PSP_DEVICE_INTERFACE_DATA iface)
{
    typedef BOOL (WINAPI *fn_t)(HDEVINFO, PCSTR, DWORD, PSP_DEVICE_INTERFACE_DATA);
    WCHAR wide[MAX_PATH];

    if (set_from(h))
    {
        MultiByteToWideChar(CP_ACP, 0, name, -1, wide, ARRAYSIZE(wide));
        return SetupDiOpenDeviceInterfaceW(h, wide, flags, iface);
    }
    return ((fn_t)orig("SetupDiOpenDeviceInterfaceA"))(h, name, flags, iface);
}

BOOL WINAPI SetupDiGetDeviceInstanceIdW(HDEVINFO h, PSP_DEVINFO_DATA info, PWSTR id,
                                        DWORD size, PDWORD required)
{
    typedef BOOL (WINAPI *fn_t)(HDEVINFO, PSP_DEVINFO_DATA, PWSTR, DWORD, PDWORD);
    struct fake_set *f = set_from(h);
    struct fake_dev *dev;

    if (f)
    {
        dev = dev_by_inst(info ? info->DevInst : 0);
        if (!dev)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        if (required)
            *required = (DWORD)((wcslen(dev->instance_id) + 1) * sizeof(WCHAR));
        if (!id || size < (wcslen(dev->instance_id) + 1) * sizeof(WCHAR))
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        wcscpy(id, dev->instance_id);
        return TRUE;
    }
    return ((fn_t)orig("SetupDiGetDeviceInstanceIdW"))(h, info, id, size, required);
}

BOOL WINAPI SetupDiGetDeviceInstanceIdA(HDEVINFO h, PSP_DEVINFO_DATA info, PSTR id,
                                        DWORD size, PDWORD required)
{
    typedef BOOL (WINAPI *fn_t)(HDEVINFO, PSP_DEVINFO_DATA, PSTR, DWORD, PDWORD);
    struct fake_set *f = set_from(h);
    struct fake_dev *dev;
    char narrow[MAX_PATH];
    DWORD need;

    if (f)
    {
        dev = dev_by_inst(info ? info->DevInst : 0);
        if (!dev)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        WideCharToMultiByte(CP_ACP, 0, dev->instance_id, -1, narrow, sizeof(narrow), NULL,
                            NULL);
        need = (DWORD)strlen(narrow) + 1;
        if (required)
            *required = need;
        if (!id || size < need)
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        strcpy(id, narrow);
        return TRUE;
    }
    return ((fn_t)orig("SetupDiGetDeviceInstanceIdA"))(h, info, id, size, required);
}

/* ------------------------------------------------------------------ */
/* registry properties */

static BOOL prop_wide(struct fake_dev *dev, DWORD prop, PDWORD type, PBYTE buf,
                      DWORD size, PDWORD required)
{
    static const WCHAR dev_hwid[] =
        L"USB\\VID_2A0E&PID_0080&REV_0000\0USB\\VID_2A0E&PID_0080\0\0";
    static const WCHAR dev_compat[] =
        L"USB\\Class_ff&SubClass_00&Prot_00\0USB\\Class_ff&SubClass_00\0USB\\Class_ff\0\0";
    static const WCHAR hub_hwid[] =
        L"USB\\ROOT_HUB30&VID_1D6B&PID_0004&REV_0000\0USB\\ROOT_HUB30\0\0";
    static const WCHAR hcd_hwid[] =
        L"PCI\\VEN_8086&DEV_1E31&SUBSYS_00000000&REV_00\0\0";
    static const WCHAR empty_multi[] = L"\0\0";
    static const WCHAR driver[] = L"{36fc9e60-c465-11cf-8056-444553540000}\\0001";
    static WCHAR wanchor[64];
    const WCHAR *str = NULL;
    DWORD str_size = 0, str_type = REG_SZ;
    static DWORD dword;
    const void *data = NULL;
    DWORD data_size = 0, data_type = REG_SZ;

    switch (prop)
    {
        case SPDRP_DEVICEDESC:
        case SPDRP_FRIENDLYNAME:
            str = dev->desc;
            break;
        case SPDRP_ENUMERATOR_NAME:
            str = dev->enumerator;
            break;
        case SPDRP_CLASS:
            str = L"USB";
            break;
        case SPDRP_CLASSGUID:
            str = CLASS_GUID_STR;
            break;
        case SPDRP_DRIVER:
            if (dev->devinst == SHIM_DEVINST_USB || dev->devinst == SHIM_DEVINST_HUB)
                str = driver;
            break;
        case SPDRP_SERVICE:
            if (dev->devinst == SHIM_DEVINST_USB)
                str = L"WinUSB";
            break;
        case SPDRP_HARDWAREID:
            if (dev->devinst == SHIM_DEVINST_USB)
            {
                data = dev_hwid;
                data_size = sizeof(dev_hwid);
                data_type = REG_MULTI_SZ;
            }
            else if (dev->devinst == SHIM_DEVINST_HUB)
            {
                data = hub_hwid;
                data_size = sizeof(hub_hwid);
                data_type = REG_MULTI_SZ;
            }
            else
            {
                data = hcd_hwid;
                data_size = sizeof(hcd_hwid);
                data_type = REG_MULTI_SZ;
            }
            break;
        case SPDRP_COMPATIBLEIDS:
            if (dev->devinst == SHIM_DEVINST_USB)
            {
                data = dev_compat;
                data_size = sizeof(dev_compat);
                data_type = REG_MULTI_SZ;
            }
            break;
        case SPDRP_UPPERFILTERS:
        case SPDRP_LOWERFILTERS:
            data = empty_multi;
            data_size = sizeof(empty_multi);
            data_type = REG_MULTI_SZ;
            break;
        case SPDRP_LOCATION_INFORMATION:
            if (dev->devinst == SHIM_DEVINST_USB)
            {
                swprintf(wanchor, ARRAYSIZE(wanchor), L"Port_#%04u.Hub_#%04u",
                         dev->addr, dev->bus);
                str = wanchor;
            }
            break;
        case SPDRP_ADDRESS:
            if (dev->devinst == SHIM_DEVINST_USB)
            {
                dword = dev->addr;
                data = &dword;
                data_size = sizeof(dword);
                data_type = REG_DWORD;
            }
            break;
        case 0x22: /* SPDRP_INSTALL_STATE */
            if (dev->devinst == SHIM_DEVINST_USB)
            {
                dword = 0;
                data = &dword;
                data_size = sizeof(dword);
                data_type = REG_DWORD;
            }
            break;
        case SPDRP_REMOVAL_POLICY:
            dword = 1; /* CM_REMOVAL_POLICY_EXPECT_NO_REMOVAL */
            data = &dword;
            data_size = sizeof(dword);
            data_type = REG_DWORD;
            break;
        default:
            break;
    }

    if (!data && str)
    {
        str_size = (DWORD)((wcslen(str) + 1) * sizeof(WCHAR));
        data = str;
        data_size = str_size;
        data_type = str_type;
    }
    if (!data)
    {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    if (type)
        *type = data_type;
    if (required)
        *required = data_size;
    if (!buf || size < data_size)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(buf, data, data_size);
    return TRUE;
}

BOOL WINAPI SetupDiGetDeviceRegistryPropertyW(HDEVINFO h, PSP_DEVINFO_DATA info,
                                              DWORD prop, PDWORD type, PBYTE buffer,
                                              DWORD size, PDWORD required)
{
    typedef BOOL (WINAPI *fn_t)(HDEVINFO, PSP_DEVINFO_DATA, DWORD, PDWORD, PBYTE, DWORD, PDWORD);
    struct fake_set *f = set_from(h);
    struct fake_dev *dev;

    if (f)
    {
        dev = dev_by_inst(info ? info->DevInst : 0);
        if (!dev)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        return prop_wide(dev, prop, type, buffer, size, required);
    }
    return ((fn_t)orig("SetupDiGetDeviceRegistryPropertyW"))(h, info, prop, type, buffer,
                                                             size, required);
}

BOOL WINAPI SetupDiGetDeviceRegistryPropertyA(HDEVINFO h, PSP_DEVINFO_DATA info,
                                              DWORD prop, PDWORD type, PBYTE buffer,
                                              DWORD size, PDWORD required)
{
    typedef BOOL (WINAPI *fn_t)(HDEVINFO, PSP_DEVINFO_DATA, DWORD, PDWORD, PBYTE, DWORD, PDWORD);
    struct fake_set *f = set_from(h);
    struct fake_dev *dev;

    if (f)
    {
        BYTE wide[1024];
        DWORD wtype = 0, wreq = 0, need = 0, i;
        dev = dev_by_inst(info ? info->DevInst : 0);
        if (!dev)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        if (!prop_wide(dev, prop, &wtype, wide, sizeof(wide), &wreq) &&
            GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            return FALSE;
        if (type)
            *type = wtype;
        if (wtype == REG_DWORD)
        {
            need = sizeof(DWORD);
            if (required)
                *required = need;
            if (!buffer || size < need)
            {
                SetLastError(ERROR_INSUFFICIENT_BUFFER);
                return FALSE;
            }
            memcpy(buffer, wide, need);
            return TRUE;
        }
        if (wtype == REG_MULTI_SZ)
        {
            char narrow[1024];
            DWORD off = 0, out = 0;
            while (off + 1 < wreq)
            {
                DWORD chunk = (DWORD)wcslen((WCHAR *)(wide + off)) + 1;
                WideCharToMultiByte(CP_ACP, 0, (WCHAR *)(wide + off), chunk, narrow + out,
                                    sizeof(narrow) - out, NULL, NULL);
                out += chunk;
                off += chunk * sizeof(WCHAR);
            }
            narrow[out++] = 0;
            need = out;
            if (required)
                *required = need;
            if (!buffer || size < need)
            {
                SetLastError(ERROR_INSUFFICIENT_BUFFER);
                return FALSE;
            }
            memcpy(buffer, narrow, need);
            return TRUE;
        }
        {
            char narrow[1024];
            WideCharToMultiByte(CP_ACP, 0, (WCHAR *)wide, -1, narrow, sizeof(narrow), NULL, NULL);
            need = (DWORD)strlen(narrow) + 1;
            if (required)
                *required = need;
            if (!buffer || size < need)
            {
                SetLastError(ERROR_INSUFFICIENT_BUFFER);
                return FALSE;
            }
            memcpy(buffer, narrow, need);
            (void)i;
            return TRUE;
        }
    }
    return ((fn_t)orig("SetupDiGetDeviceRegistryPropertyA"))(h, info, prop, type, buffer,
                                                             size, required);
}

static HKEY open_enum_key(struct fake_dev *dev, REGSAM access)
{
    WCHAR subkey[256];
    HKEY key;

    if (dev->devinst == SHIM_DEVINST_USB)
        wcscpy(subkey, L"System\\CurrentControlSet\\Enum\\USB\\VID_2A0E&PID_0080\\8&6");
    else if (dev->devinst == SHIM_DEVINST_HUB)
        wcscpy(subkey, L"System\\CurrentControlSet\\Enum\\USB\\ROOT_HUB30\\4&0&0");
    else
        wcscpy(subkey, L"System\\CurrentControlSet\\Enum\\PCI\\VEN_8086&DEV_1E31\\0");

    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, subkey, 0, NULL, 0, access, NULL, &key, NULL)
        != ERROR_SUCCESS)
        return INVALID_HANDLE_VALUE;

    {
        static const WCHAR classguid[] = L"{36FC9E60-C465-11CF-8056-444553540000}";
        static const WCHAR hwid[] =
            L"USB\\VID_2A0E&PID_0080&REV_0000\0USB\\VID_2A0E&PID_0080\0\0";
        RegSetValueExW(key, L"DeviceDesc", 0, REG_SZ, (const BYTE *)dev->desc,
                       (DWORD)((wcslen(dev->desc) + 1) * sizeof(WCHAR)));
        RegSetValueExW(key, L"ClassGUID", 0, REG_SZ, (const BYTE *)classguid,
                       sizeof(classguid));
        RegSetValueExW(key, L"Class", 0, REG_SZ, (const BYTE *)L"USB", sizeof(L"USB"));
        RegSetValueExW(key, L"Enumerator", 0, REG_SZ, (const BYTE *)dev->enumerator,
                       (DWORD)((wcslen(dev->enumerator) + 1) * sizeof(WCHAR)));
        if (dev->devinst == SHIM_DEVINST_USB)
        {
            static const WCHAR service[] = L"WinUSB";
            static const WCHAR guid_multi[] =
                L"{DEE824EF-729B-4A0E-9C14-B7117D33A817}\0\0";
            RegSetValueExW(key, L"Service", 0, REG_SZ, (const BYTE *)service,
                           sizeof(service));
            RegSetValueExW(key, L"HardwareID", 0, REG_MULTI_SZ, (const BYTE *)hwid,
                           sizeof(hwid));
            RegSetValueExW(key, L"DeviceInterfaceGUIDs", 0, REG_MULTI_SZ,
                           (const BYTE *)guid_multi, sizeof(guid_multi));
        }
    }
    return key;
}

HKEY WINAPI SetupDiOpenDevRegKey(HDEVINFO h, PSP_DEVINFO_DATA info, DWORD scope,
                                 DWORD profile, DWORD key_type, REGSAM access)
{
    typedef HKEY (WINAPI *fn_t)(HDEVINFO, PSP_DEVINFO_DATA, DWORD, DWORD, DWORD, REGSAM);
    struct fake_set *f = set_from(h);
    struct fake_dev *dev;

    (void)scope; (void)profile;
    if (f)
    {
        dev = dev_by_inst(info ? info->DevInst : 0);
        if (!dev)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return INVALID_HANDLE_VALUE;
        }
        if (key_type == 2) /* DIREG_DRV */
        {
            static const WCHAR desc[] = L"WinUSB device";
            static const WCHAR provider[] = L"DreamSourceLab";
            static const WCHAR infpath[] = L"winusb.inf";
            DWORD ver = 0x00010000;
            HKEY key;

            if (RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                                L"System\\CurrentControlSet\\Control\\Class\\"
                                L"{36fc9e60-c465-11cf-8056-444553540000}\\0001",
                                0, NULL, 0, access, NULL, &key, NULL) != ERROR_SUCCESS)
                return INVALID_HANDLE_VALUE;
            RegSetValueExW(key, L"DriverDesc", 0, REG_SZ, (const BYTE *)desc, sizeof(desc));
            RegSetValueExW(key, L"ProviderName", 0, REG_SZ, (const BYTE *)provider,
                           sizeof(provider));
            RegSetValueExW(key, L"Class", 0, REG_SZ, (const BYTE *)L"USB", sizeof(L"USB"));
            RegSetValueExW(key, L"DriverVersion", 0, REG_DWORD, (const BYTE *)&ver, sizeof(ver));
            RegSetValueExW(key, L"InfPath", 0, REG_SZ, (const BYTE *)infpath, sizeof(infpath));
            return key;
        }
        return open_enum_key(dev, access);
    }
    return ((fn_t)orig("SetupDiOpenDevRegKey"))(h, info, scope, profile, key_type, access);
}

HKEY WINAPI SetupDiOpenDeviceInterfaceRegKey(HDEVINFO h, PSP_DEVICE_INTERFACE_DATA iface,
                                             DWORD reserved, REGSAM access)
{
    typedef HKEY (WINAPI *fn_t)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, DWORD, REGSAM);
    struct fake_set *f = set_from(h);

    (void)reserved;
    if (f)
    {
        struct fake_dev *dev = dev_by_inst((DWORD)(iface ? iface->Reserved : 0));
        if (!dev)
            dev = &devices[2];
        return open_enum_key(dev, access);
    }
    return ((fn_t)orig("SetupDiOpenDeviceInterfaceRegKey"))(h, iface, reserved, access);
}

/* ------------------------------------------------------------------ */

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        WCHAR exe[MAX_PATH];
        WSADATA wsa;

        InitializeCriticalSection(&lock);
        WSAStartup(MAKEWORD(2, 2), &wsa);
        CreateDirectoryA("C:\\ProgramData\\DSWaveShim", NULL);
        init_devices();
        InterlockedExchange(&initialized, 1);

        GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe));
        if (wcsstr(exe, L"DSWave.exe"))
        {
            shim_log("setupapi shim loaded in DSWave (pid %lu)",
                     (unsigned long)GetCurrentProcessId());
            ioctl_hook_install();
        }
    }
    return TRUE;
}
