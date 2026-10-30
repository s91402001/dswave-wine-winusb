/* Replay the FPGA bitstream upload for DSCope M454.
 * Usage: dswfpga [reset] [chunk]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libusb.h>

static int ctl(libusb_device_handle *h, int req, const char *d, int n)
{
    unsigned char b[4] = {0, 0, 0, 0};
    int r;
    memcpy(b, d, n);
    r = libusb_control_transfer(h, 0x40, req, 0, 0, b, n, 5000);
    printf("ctl %02x %02x %02x %02x -> %d\n", req, b[0], b[1], b[2], r);
    return r;
}

int main(int argc, char **argv)
{
    int do_reset = argc > 1 ? atoi(argv[1]) : 0;
    int chunk = argc > 2 ? atoi(argv[2]) : 0;
    libusb_device **list = NULL;
    libusb_device_handle *h = NULL;
    unsigned char *buf;
    long len;
    FILE *f;
    ssize_t n, i;
    int r, found = 0;

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

    ctl(h, 0xb1, "\xf0\x02\x00", 3);
    ctl(h, 0xb1, "\xf2\x02\x00", 3);
    ctl(h, 0xb1, "\xff\x02\x00", 3);
    ctl(h, 0xb0, "\xfe\xfe\xff", 3);
    ctl(h, 0xb0, "\xfe\x01\x00", 3);
    ctl(h, 0xb1, "\xff\x02\x00", 3);
    ctl(h, 0xb0, "\xfc\xe8\x1d", 3);
    ctl(h, 0xb0, "\xfd\x07\x00", 3);

    if (chunk <= 0) {
        int xfer = 0;
        r = libusb_bulk_transfer(h, 0x02, buf, (int)len, &xfer, 30000);
        printf("upload(one) -> %d %s xfer=%d/%ld\n", r, libusb_error_name(r), xfer, len);
    } else {
        long off = 0;
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
    ctl(h, 0xb1, "\xff\x02\x00", 3);
    ctl(h, 0xb1, "\xf1\x02\x00", 3);
    ctl(h, 0xb1, "\xf0\x02\x00", 3);

    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(NULL);
    free(buf);
    return 0;
}
