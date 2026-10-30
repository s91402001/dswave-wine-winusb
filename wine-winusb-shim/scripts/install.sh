#!/bin/bash
# Install the WinUSB shim into a Wine prefix and start the Linux helper.
set -euo pipefail

PREFIX="${WINEPREFIX:-$HOME/.wine-dswave}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SYS="$PREFIX/drive_c/windows/system32"
APP="$PREFIX/drive_c/Program Files/DSWave"
WINE_LIB=/opt/wine-devel/lib/wine/x86_64-windows

[ -d "$PREFIX" ] || { echo "no such prefix: $PREFIX" >&2; exit 1; }
[ -d "$WINE_LIB" ] || { echo "missing $WINE_LIB" >&2; exit 1; }

echo "== backup originals (from Wine, not from an already-shimmed system32) =="
[ -f "$SYS/setupapi_wine.dll" ] || cp -f "$WINE_LIB/setupapi.dll" "$SYS/setupapi_wine.dll"
[ -f "$SYS/setupapi_orig.dll" ] || cp -f "$WINE_LIB/setupapi.dll" "$SYS/setupapi_orig.dll"
[ -f "$SYS/winusb_wine.dll" ]   || cp -f "$WINE_LIB/winusb.dll"   "$SYS/winusb_wine.dll"
[ -f "$SYS/cfgmgr32_wine.dll" ] || cp -f "$WINE_LIB/cfgmgr32.dll" "$SYS/cfgmgr32_wine.dll"
[ -f "$SYS/cfgmgr32_orig.dll" ] || cp -f "$WINE_LIB/cfgmgr32.dll" "$SYS/cfgmgr32_orig.dll"

echo "== remove stale copies from the app directory =="
rm -f "$APP/setupapi.dll" "$APP/winusb.dll" "$APP/WinUSB.dll" "$APP/setupapi.dll.shim"

echo "== install shims =="
cp -f "$HERE/build/setupapi.dll" "$SYS/setupapi.dll"
cp -f "$HERE/build/cfgmgr32.dll" "$SYS/cfgmgr32.dll"
cp -f "$HERE/build/winusb.dll"   "$SYS/winusb.dll"

echo "== dll overrides =="
export WINEPREFIX="$PREFIX"
wineserver -k 2>/dev/null || true
wine reg add 'HKCU\Software\Wine\DllOverrides' /v setupapi /d native /f >/dev/null
wine reg add 'HKCU\Software\Wine\DllOverrides' /v cfgmgr32 /d native /f >/dev/null
wine reg add 'HKCU\Software\Wine\DllOverrides' /v winusb  /d native /f >/dev/null
wine reg delete 'HKLM\System\CurrentControlSet\Control\Session Manager\KnownDLLs' /v setupapi /f >/dev/null 2>&1 || true

echo "== helper =="
pkill -x helper 2>/dev/null || true
setsid nohup "$HERE/build/helper" > /tmp/dswave-helper.log 2>&1 < /dev/null &
sleep 0.5
echo "installed into $PREFIX (helper log: /tmp/dswave-helper.log)"
