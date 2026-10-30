<#
.SYNOPSIS
    Install Wireshark + USBPcap (run once as Administrator).
#>

$ErrorActionPreference = 'Stop'

Write-Host '== Checking Wireshark / USBPcap =='
$dumpcap = 'C:\Program Files\Wireshark\dumpcap.exe'
if (Test-Path $dumpcap) {
    Write-Host "Already installed: $dumpcap"
} else {
    Write-Host 'Wireshark not found. Trying winget (select "Install USBPcap" if prompted)...'
    if (Get-Command winget -ErrorAction SilentlyContinue) {
        winget install --id WiresharkFoundation.Wireshark -e `
            --accept-source-agreements --accept-package-agreements
    } elseif (Get-Command choco -ErrorAction SilentlyContinue) {
        choco install wireshark -y
    } else {
        Write-Error 'Neither winget nor choco found. Install Wireshark manually (with USBPcap).'
    }
}

if (-not (Test-Path $dumpcap)) {
    Write-Error "dumpcap still missing: $dumpcap"
}

Write-Host '== Capture interfaces =='
& $dumpcap -D

Write-Host 'Done. Next: 02-capture.ps1'
