/* Replay the FPGA bitstream upload for DSCope M454 (v3).
 * v2 finding (docs/analysis/fpga-upload-sequence.md run notes): the device
 * has no EP 0x82 (only 0x02 OUT / 0x86 IN / 0x84 IN), and a zero-length bulk
 * transfer never reaches the wire, so the "ZLP IN" is not the missing step.
 * What matters: Windows always reads b2 == 03 00 (idle, unconfigured) right
 * before PROG, with ~15 ms between every command.
 * v3: inter-command delays, wait for the 03 idle state before PROG, and up to
 * N full retries of the whole sequence.
 * Usage: dswfpga [reset] [chunk] [attempts]
 * Exit 0 = FPGA configured (b2 0x80 bit seen), 1 = not configured/error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb.h>

static void ms(int t) { usleep((useconds_t)t * 1000u); }

static int ctl(libusb_device_handle *h, int req, const char *d, int n)
{
    unsigned char b[4] = {0, 0, 0, 0};
    int r;
    memcpy(b, d, n);
    r = libusb_control_transfer(h, 0x40, req, 0, 0, b, n, 5000);
    printf("ctl %02x %02x %02x %02x -> %d\n", req, b[0], b[1], b[2], r);
    ms(15); /* Windows commands are ~15 ms apart */
    return r;
}

/* Vendor status read c0 b2, 2 bytes. byte0: 03 = idle/unconfigured,
 * bit 0x80 = configured. Returns byte0 or -1. */
static int st2(libusb_device_handle *h, const char *tag)
{
    unsigned char b[2] = {0, 0};
    int r = libusb_control_transfer(h, 0xC0, 0xB2, 0, 0, b, 2, 5000);
    printf("b2 %s -> %d", tag, r);
    if (r == 2) printf(" data=%02x %02x%s", b[0], b[1], (b[0] & 0x80) ? " [CONFIGURED]" : "");
    printf("\n");
    ms(15);
    if (r == 2) return b[0];
    return -1;
}

/* one full attempt: pre-sequence -> PROG -> upload -> completion poll */
static int attempt(libusb_device_handle *h, unsigned char *buf, long len, int chunk)
{
    int r, configured = 0, i;
    int s;

    ctl(h, 0xb1, "\xf0\x02\x00", 3); st2(h, "after f0");
    ctl(h, 0xb1, "\xf2\x02\x00", 3); st2(h, "after f2");
    ctl(h, 0xb1, "\xff\x02\x00", 3); st2(h, "after ff");
    ctl(h, 0xb0, "\xfe\xfe\xff", 3);
    ctl(h, 0xb0, "\xfe\x01\x00", 3);
    ctl(h, 0xb1, "\xff\x02\x00", 3);
    st2(h, "before prog");
    /* hard precondition: the device must report idle (03) before PROG;
     * programming while busy (01) is what left the FPGA unconfigured in v1. */
    {
        int idle = 0;
        for (i = 0; i < 10; i++)
        {
            s = st2(h, "idle poll");
            if (s == 0x03) { idle = 1; break; }
            ms(1000);
        }
        if (!idle)
        {
            printf("-- idle wait timed out (b2=%d), skipping PROG --\n", s);
            return 0;
        }
    }
    ctl(h, 0xb0, "\xfc\xe8\x1d", 3);   /* FPGA PROG pulse */
    ctl(h, 0xb0, "\xfd\x07\x00", 3);   /* FPGA init start */

    if (chunk <= 0) {
        int xfer = 0;
        r = libusb_bulk_transfer(h, 0x02, buf, (int)len, &xfer, 30000);
        printf("upload(one) -> %d %s xfer=%d/%ld\n", r, libusb_error_name(r), xfer, len);
    } else {
        long off = 0;
        r = 0;
        while (off < len) {
            int want = (len - off) < chunk ? (int)(len - off) : chunk;
            int xfer = 0;
            r = libusb_bulk_transfer(h, 0x02, buf + off, want, &xfer, 30000);
            printf("upload chunk @%ld want=%d -> %d %s xfer=%d\n", off, want, r,
                   r ? libusb_error_name(r) : "ok", xfer);
            if (r) break;
            off += xfer;
            if (xfer < want) { printf("short write\n"); break; }
        }
    }
    if (r) return 0;

    /* completion: b2 must show the 0x80 bit (Windows: 03 00 -> 83 00) */
    ctl(h, 0xb1, "\xff\x02\x00", 3);
    for (i = 0; i < 10; i++) {
        s = st2(h, "poll");
        if (s >= 0 && (s & 0x80)) { configured = 1; break; }
        ms(1000);
    }
    if (configured) {
        ctl(h, 0xb1, "\xf1\x02\x00", 3);
        st2(h, "after f1");
    }
    return configured;
}

int main(int argc, char **argv)
{
    int do_reset = argc > 1 ? atoi(argv[1]) : 0;
    int chunk = argc > 2 ? atoi(argv[2]) : 0;
    int attempts = argc > 3 ? atoi(argv[3]) : 3;
    libusb_device **list = NULL;
    libusb_device_handle *h = NULL;
    unsigned char *buf;
    long len;
    FILE *f;
    ssize_t n, i;
    int r, found = 0, configured = 0;

    f = fopen("/tmp/dswave-fpga.bin", "rb");
    if (!f) { perror("bitstream"); return 1; }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(len);
    if (!buf || fread(buf, 1, len, f) != (size_t)len) { printf("read failed\n"); return 1; }
    fclose(f);
    printf("bitstream %ld bytes\n", len);

    libusb_init(NULL);
    libusb_set_option(NULL, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);
    n = libusb_get_device_list(NULL, &list);
    for (i = 0; i < n; i++) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d)) continue;
        if (d.idVendor == 0x2a0e && d.idProduct == 0x0080) {
            r = libusb_open(list[i], &h);
            printf("open %u:%u -> %d (%s)\n", libusb_get_bus_number(list[i]),
                   libusb_get_device_address(list[i]), r, r ? libusb_error_name(r) : "ok");
            found = 1;
            break;
        }
    }
    libusb_free_device_list(list, 1);
    if (!found || !h) { printf("device not found\n"); return 1; }

    if (do_reset) {
        r = libusb_reset_device(h);
        printf("reset -> %d (%s)\n", r, libusb_error_name(r));
    }
    libusb_set_configuration(h, 1);
    r = libusb_claim_interface(h, 0);
    printf("claim -> %d (%s)\n", r, libusb_error_name(r));
    if (r) return 1;

    for (i = 0; i < attempts; i++) {
        printf("== attempt %d/%d ==\n", (int)i + 1, attempts);
        if (attempt(h, buf, len, chunk)) { configured = 1; break; }
        if (i + 1 < attempts) {
            printf("-- retrying after programmer stop --\n");
            ctl(h, 0xb1, "\xf2\x02\x00", 3);
            ms(1000); /* ms() takes milliseconds: 1 s, not 985000 */
        }
    }

    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(NULL);
    free(buf);
    printf("RESULT: %s\n", configured ? "FPGA CONFIGURED" : "NOT configured");
    return configured ? 0 : 1;
}
