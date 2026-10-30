<#
.SYNOPSIS
    Extract control.csv / bulk.csv / summary.txt from a USBPcap pcap file.

.EXAMPLE
    .\03-extract.ps1 -Pcap C:\usbpcap\capture-20261030-091500.pcap
#>

param(
    [Parameter(Mandatory = $true)][string]$Pcap,
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'

$tshark = 'C:\Program Files\Wireshark\tshark.exe'
if (-not (Test-Path $tshark)) { throw "tshark not found: $tshark" }
if (-not (Test-Path $Pcap))   { throw "pcap not found: $Pcap" }
if (-not $OutDir) { $OutDir = Split-Path -Parent $Pcap }

$raw = Join-Path $OutDir 'usb-raw.csv'
$ctl = Join-Path $OutDir 'control.csv'
$blk = Join-Path $OutDir 'bulk.csv'
$sum = Join-Path $OutDir 'summary.txt'

$fields = @(
    'frame.number', 'frame.time_relative',
    'usb.endpoint_address', 'usb.transfer_type', 'usb.data_len',
    'usb.capdata', 'usb.urb_status',
    'usb.setup.bmRequestType', 'usb.setup.bRequest',
    'usb.setup.wValue', 'usb.setup.wIndex', 'usb.setup.wLength'
)

Write-Host "== tshark extract: $Pcap =="
$args = @('-r', $Pcap, '-Y', 'usb.idVendor == 0x2a0e', '-T', 'fields')
foreach ($f in $fields) { $args += @('-e', $f) }
$args += @('-E', 'header=y', '-E', 'separator=,', '-E', 'quote=d')
& $tshark @args | Set-Content -Encoding UTF8 $raw

$lines = Get-Content $raw
if ($lines.Count -le 1) {
    Write-Warning 'No 2a0e packets captured. Check:'
    Write-Warning '  1. Device passed through to the VM'
    Write-Warning '  2. Correct USBPcap interface (dumpcap -D)'
    Write-Warning '  3. Run was pressed during the capture'
    exit 1
}

$rows = $lines | ConvertFrom-Csv
Write-Host "USB packets: $($rows.Count)"

$control = $rows | Where-Object { $_.'usb.transfer_type' -eq '0x00000002' -or $_.'usb.transfer_type' -eq '2' }
$bulk    = $rows | Where-Object { $_.'usb.transfer_type' -eq '0x00000003' -or $_.'usb.transfer_type' -eq '3' }

$control | Export-Csv -NoTypeInformation -Encoding UTF8 $ctl
$bulk    | Export-Csv -NoTypeInformation -Encoding UTF8 $blk

$out = @()
$out += "pcap: $Pcap"
$out += "control packets: $($control.Count)"
$out += "bulk packets: $($bulk.Count)"
$out += ''
$out += '== bulk by endpoint / length =='
$out += $bulk | Group-Object 'usb.endpoint_address', 'usb.data_len' |
        Sort-Object Count -Descending |
        ForEach-Object { "  $($_.Name)  x$($_.Count)" }
$out += ''
$out += '== bulk errors =='
$out += $bulk | Where-Object { $_.'usb.urb_status' -and $_.'usb.urb_status' -ne '0x00000000' -and $_.'usb.urb_status' -ne '0' } |
        ForEach-Object { "  frame $($_.'frame.number') ep $($_.'usb.endpoint_address') status $($_.'usb.urb_status') len $($_.'usb.data_len')" }
$out | Set-Content -Encoding UTF8 $sum

Write-Host "Output:"
Write-Host "  $ctl"
Write-Host "  $blk"
Write-Host "  $sum"
Get-Content $sum
