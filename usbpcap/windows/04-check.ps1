<#
.SYNOPSIS
    Check that Wireshark + USBPcap are installed and usable (run as Administrator).
#>

$ErrorActionPreference = 'Continue'

function Section($t) { Write-Host "`n== $t ==" -ForegroundColor Cyan }

Section 'Files'
$paths = @(
    'C:\Program Files\Wireshark\dumpcap.exe',
    'C:\Program Files\Wireshark\tshark.exe',
    'C:\Program Files\USBPcap\USBPcapCMD.exe',
    'C:\Windows\System32\drivers\USBPcap.sys'
)
foreach ($p in $paths) {
    '{0,-55} {1}' -f $p, (Test-Path $p)
}

Section 'Service'
Get-Service -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match 'USBPcap' -or $_.DisplayName -match 'USBPcap' } |
    Format-Table -AutoSize

Section 'PnP devices'
$pnp = Get-PnpDevice -ErrorAction SilentlyContinue |
    Where-Object { $_.FriendlyName -match 'USBPcap' -or $_.InstanceId -match 'USBPcap' }
if ($pnp) { $pnp | Format-Table -AutoSize } else { Write-Host '(no USBPcap PnP device found)' }

Section 'dumpcap -D'
$dumpcap = 'C:\Program Files\Wireshark\dumpcap.exe'
if (Test-Path $dumpcap) {
    & $dumpcap -D
} else {
    Write-Host 'dumpcap not found'
}

Section 'Hints'
if (-not (Test-Path 'C:\Windows\System32\drivers\USBPcap.sys')) {
    Write-Host 'USBPcap driver is missing. Install USBPcap separately and REBOOT:'
    Write-Host '  https://github.com/desowin/usbpcap/releases'
} else {
    Write-Host 'Driver exists. If dumpcap -D still shows no USBPcapN:'
    Write-Host '  1. Run as Administrator'
    Write-Host '  2. Reboot after the first driver install'
    Write-Host '  3. USB 3.0 (xHCI) controllers may not be supported by older USBPcap;'
    Write-Host '     try adding a USB 2.0 controller to the VM'
    Write-Host '  4. Or use Linux usbmon (see README)'
}
