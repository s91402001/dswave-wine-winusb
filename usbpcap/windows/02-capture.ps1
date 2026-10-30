<#
.SYNOPSIS
    Capture USB traffic with USBPcapCMD (run as Administrator).

.DESCRIPTION
    Uses USBPcap's own command line tool (no Wireshark/dumpcap).

    - Starts USBPcapCMD in the foreground with -A (all root hubs)
    - Launches DSWave a few seconds later via a background job
    - You stop the capture with Ctrl-C in this window (graceful, keeps the pcap valid)
    - Then verifies the .pcap file

.PARAMETER Output
    Output pcap path (default C:\usbpcap\fpga.pcap).

.PARAMETER Device
    Optional single device, e.g. \\.\USBPcap1. Empty = -A (all devices).

.EXAMPLE
    .\02-capture.ps1
    .\02-capture.ps1 -Device \\.\USBPcap1 -Output C:\usbpcap\fpga.pcap
#>

param(
    [string]$Output = 'C:\usbpcap\fpga.pcap',
    [string]$Device = ''
)

$ErrorActionPreference = 'Stop'

$usbpcap = 'C:\Program Files\USBPcap\USBPcapCMD.exe'
$dswave  = 'C:\Program Files\DSWave\DSWave.exe'
if (-not (Test-Path $usbpcap)) { throw "USBPcapCMD not found: $usbpcap" }
if (-not (Test-Path $dswave))  { throw "DSWave not found: $dswave" }

$isAdmin = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Warning 'Not running as Administrator - USBPcap capture will fail.'
}

$dir = Split-Path -Parent $Output
if ($dir) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
if (Test-Path $Output) { Remove-Item $Output -Force }

if ($Device) {
    $cmdArgs = @('-d', $Device, '-o', $Output)
} else {
    $cmdArgs = @('-A', '-o', $Output)
}

Write-Host "USBPcapCMD $($cmdArgs -join ' ')"
Write-Host 'DSWave will start in 3 seconds.'
Write-Host ''
Write-Host 'When DSWave has finished the FPGA configure step (or you have captured enough),'
Write-Host 'press Ctrl-C IN THIS WINDOW to stop the capture.'
Write-Host ''

Start-Job -ScriptBlock {
    param($exe)
    Start-Sleep -Seconds 3
    Start-Process -FilePath $exe
} -ArgumentList $dswave | Out-Null

& $usbpcap @cmdArgs

if (Test-Path $Output) {
    $size = (Get-Item $Output).Length
    Write-Host "Done: $Output ($size bytes)"
    if ($size -lt 1024) {
        Write-Warning 'File is very small - probably no traffic captured.'
    }
} else {
    Write-Warning 'Output file was not created.'
}
