#!/bin/bash
# Build the DSWave Wine WinUSB shim (PE DLLs + Linux helper).
set -euo pipefail

cd "$(dirname "$0")/.."
SRC=src
BUILD=build
WINE_LIB=/opt/wine-devel/lib/wine/x86_64-windows

mkdir -p "$BUILD"

echo "== helper =="
gcc -O2 -Wall -o "$BUILD/helper" "$SRC/helper.c" -lusb-1.0 -lpthread

SETUPAPI_IMPL="SetupDiGetClassDevsW,SetupDiGetClassDevsA,SetupDiGetClassDevsExW,SetupDiGetClassDevsExA,SetupDiDestroyDeviceInfoList,SetupDiCreateDeviceInfoList,SetupDiEnumDeviceInfo,SetupDiEnumDeviceInterfaces,SetupDiGetDeviceInterfaceDetailW,SetupDiGetDeviceInterfaceDetailA,SetupDiOpenDeviceInterfaceW,SetupDiOpenDeviceInterfaceA,SetupDiGetDeviceInstanceIdW,SetupDiGetDeviceInstanceIdA,SetupDiGetDeviceRegistryPropertyW,SetupDiGetDeviceRegistryPropertyA,SetupDiOpenDevRegKey,SetupDiOpenDeviceInterfaceRegKey,shim_lookup_iocp,shim_forget_iocp"
CFGMGR_IMPL="CM_Get_Parent,CM_Get_Parent_Ex,CM_Get_Child,CM_Get_Child_Ex"

echo "== setupapi.def =="
python3 scripts/gen_def.py "$WINE_LIB/setupapi.dll" "$BUILD/setupapi.def" setupapi_orig "$SETUPAPI_IMPL"

echo "== setupapi.dll =="
x86_64-w64-mingw32-gcc -O2 -Wall -Wno-attributes -shared \
    -o "$BUILD/setupapi.dll" "$SRC/setupapi_shim.c" "$SRC/ioctl_hook.c" \
    "$BUILD/setupapi.def" -lws2_32 -static-libgcc

echo "== cfgmgr32.def =="
python3 scripts/gen_def.py "$WINE_LIB/cfgmgr32.dll" "$BUILD/cfgmgr32.def" cfgmgr32_orig "$CFGMGR_IMPL"

echo "== cfgmgr32.dll =="
x86_64-w64-mingw32-gcc -O2 -Wall -Wno-attributes -shared \
    -o "$BUILD/cfgmgr32.dll" "$SRC/cfgmgr32_shim.c" "$BUILD/cfgmgr32.def" -static-libgcc

echo "== winusb.dll =="
x86_64-w64-mingw32-gcc -O2 -Wall -Wno-attributes -shared \
    -o "$BUILD/winusb.dll" "$SRC/winusb_shim.c" -lws2_32 -static-libgcc \
    -Wl,--export-all-symbols

echo "done:"
ls -l "$BUILD"/setupapi.dll "$BUILD"/cfgmgr32.dll "$BUILD"/winusb.dll "$BUILD"/helper
