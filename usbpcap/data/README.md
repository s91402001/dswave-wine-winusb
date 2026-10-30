# Reference captures

USB captures taken on **Windows 11 (VM)** with `USBPcapCMD.exe`, used as the
behavioural baseline for the Wine shim. Stored gzip-compressed because the raw
`output.pcap` (129 MB) exceeds GitHub's 100 MB per-file limit.

| File | Uncompressed | Compressed | Contents |
|---|---|---|---|
| `fpga1.pcap.gz` | 8,920,535 B | 2,004,924 B | replug → DSWave start → FPGA configure → run → stop (13.5 s) |
| `output.pcap.gz` | 129,023,508 B | 29,822,271 B | normal capture session (USBPcap, full frames) |

Decompress:

```bash
gunzip -k usbpcap/data/fpga1.pcap.gz
python3 usbpcap/linux/parse-usbpcap.py usbpcap/data/fpga1.pcap --summary
```

## Integrity (sha256)

```
155b3861395faca44a697553ec51c346f8b4f7a3ce71e76563a38a9656133e5f  fpga1.pcap
515f0837e83a0bc6e3cf2dbcca8efda8e2647405a41e09a44ae1cab3173d47aa  output.pcap
c44df30c0ce9c566bd96b689c085a53b38109caae340c5fb576df73dc24ff289  fpga1.pcap.gz
16a08a593ed921fb281caaa7188435953675339be03cb244f5404b4a8193c53e  output.pcap.gz
```

## What the captures show

`fpga1.pcap` (the important one):

* FPGA bitstream upload: one bulk OUT on `0x02`, **466,408 bytes**, status 0,
  immediately followed by a zero-length `0x02` IN.
* Capture data: `0x86` IN **405,504 bytes × 133**, ~22 ms apart (≈18.6 MB/s),
  preceded by a 4096-byte header (`55 55 55 55 01 00 00 00`).
* Stop: pending read ends with **`0xC0010000` (USBD_STATUS_CANCELED)**, `len=0`.
* Sequences:
  * start `03 01 00` → `02 00 00` → `01 01 00`
  * stop `01 00 00` → `02 08 00` → (`0x84` out0/in1 ×3) → `01 00 00` → `02 04 00`

Recording instructions: [`../README.md`](../README.md).
