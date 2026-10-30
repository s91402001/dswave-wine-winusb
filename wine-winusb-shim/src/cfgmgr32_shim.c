/*
 * cfgmgr32.dll shim: supplies the fake parent/child topology that libusb walks
 * while it builds its device tree.  Everything else is forwarded to the real
 * Wine implementation (cfgmgr32_orig.dll).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cfgmgr32.h>
#include <stdio.h>

#include "shim_ids.h"

static HMODULE orig_module;

static FARPROC orig(const char *name)
{
    if (!orig_module)
        orig_module = LoadLibraryA("C:\\windows\\system32\\cfgmgr32_orig.dll");
    if (!orig_module)
        return NULL;
    return GetProcAddress(orig_module, name);
}

static void shim_log(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    FILE *f;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    f = fopen("C:\\ProgramData\\DSWaveShim\\cfgmgr32.log", "a");
    if (f)
    {
        fprintf(f, "%s\n", buf);
        fclose(f);
    }
}

CONFIGRET WINAPI CM_Get_Parent(PDWORD parent, DWORD devinst, ULONG flags)
{
    typedef CONFIGRET (WINAPI *fn_t)(PDWORD, DWORD, ULONG);

    shim_log("CM_Get_Parent devinst=%#lx", (unsigned long)devinst);
    switch (devinst)
    {
        case SHIM_DEVINST_USB:
            if (parent)
                *parent = SHIM_DEVINST_HUB;
            return CR_SUCCESS;
        case SHIM_DEVINST_HUB:
        case SHIM_DEVINST_HCD:
            return CR_NO_SUCH_DEVNODE;
        default:
            break;
    }
    if (orig("CM_Get_Parent"))
        return ((fn_t)orig("CM_Get_Parent"))(parent, devinst, flags);
    return CR_NO_SUCH_DEVNODE;
}

CONFIGRET WINAPI CM_Get_Parent_Ex(PDWORD parent, DWORD devinst, ULONG flags, HMACHINE machine)
{
    typedef CONFIGRET (WINAPI *fn_t)(PDWORD, DWORD, ULONG, HMACHINE);

    (void)machine;
    return CM_Get_Parent(parent, devinst, flags);
}

CONFIGRET WINAPI CM_Get_Child(PDWORD child, DWORD devinst, ULONG flags)
{
    typedef CONFIGRET (WINAPI *fn_t)(PDWORD, DWORD, ULONG);

    shim_log("CM_Get_Child devinst=%#lx", (unsigned long)devinst);
    switch (devinst)
    {
        case SHIM_DEVINST_HCD:
            if (child)
                *child = SHIM_DEVINST_HUB;
            return CR_SUCCESS;
        case SHIM_DEVINST_HUB:
            if (child)
                *child = SHIM_DEVINST_USB;
            return CR_SUCCESS;
        case SHIM_DEVINST_USB:
            return CR_NO_SUCH_DEVNODE;
        default:
            break;
    }
    if (orig("CM_Get_Child"))
        return ((fn_t)orig("CM_Get_Child"))(child, devinst, flags);
    return CR_NO_SUCH_DEVNODE;
}

CONFIGRET WINAPI CM_Get_Child_Ex(PDWORD child, DWORD devinst, ULONG flags, HMACHINE machine)
{
    (void)machine;
    return CM_Get_Child(child, devinst, flags);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        CreateDirectoryA("C:\\ProgramData\\DSWaveShim", NULL);
    return TRUE;
}
