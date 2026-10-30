#!/bin/bash
# Remove the WinUSB shim from a Wine prefix.
set -euo pipefail

PREFIX="${WINEPREFIX:-$HOME/.wine-dswave}"
SYS="$PREFIX/drive_c/windows/system32"

[ -d "$PREFIX" ] || { echo "no such prefix: $PREFIX" >&2; exit 1; }

export WINEPREFIX="$PREFIX"
wineserver -k 2>/dev/null || true

for n in setupapi winusb cfgmgr32; do
    if [ -f "$SYS/${n}_wine.dll" ]; then
        cp -f "$SYS/${n}_wine.dll" "$SYS/${n}.dll"
    fi
    wine reg delete 'HKCU\Software\Wine\DllOverrides' /v "$n" /f >/dev/null 2>&1 || true
done

wine reg add 'HKLM\System\CurrentControlSet\Control\Session Manager\KnownDLLs' \
    /v setupapi /t REG_SZ /d setupapi.dll /f >/dev/null 2>&1 || true

pkill -x helper 2>/dev/null || true
echo "uninstalled from $PREFIX"
