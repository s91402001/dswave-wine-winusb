/* Standalone bulk-IN test for DSCope M454 (no Wine, no app).
 * Usage: dswbulk [seconds] [mbps] [chunk] [poll84]
 *   seconds : run time
 *   mbps    : pacing cap (0 = unlimited)
 *   chunk   : libusb request size; frames are 5005312 bytes
 *   poll84  : 1 = also poll ep 0x84 (1 byte, 200ms) like the app
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <libusb.h>
#include "dswcfg.h"

static volatile int running = 1;
static libusb_device_handle *gh;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int ctl(libusb_device_handle *h, const unsigned char *d, int n)
{
    int r = libusb_control_transfer(h, 0x40, 0xb0, 0, 0, (unsigned char *)d, n, 5000);
    printf("ctl %02x %02x %02x -> %d\n", d[0], d[1], d[2], r);
    return r;
}

static void *poll_thread(void *arg)
{
    unsigned char c;
    long polls = 0, timeouts = 0, errors = 0;
    (void)arg;
    while (running)
    {
        int xfer = 0;
        int r = libusb_bulk_transfer(gh, 0x84, &c, 1, &xfer, 200);
        polls++;
        if (r == LIBUSB_ERROR_TIMEOUT)
            timeouts++;
        else if (r)
        {
            errors++;
            printf("poll84 error: %d %s\n", r, libusb_error_name(r));
            break;
        }
    }
    printf("poll84: %ld polls, %ld timeouts, %ld errors\n", polls, timeouts, errors);
    return NULL;
}

int main(int argc, char **argv)
{
    double max_s = argc > 1 ? atof(argv[1]) : 30.0;
    double mbps = argc > 2 ? atof(argv[2]) : 20.0;
    int chunk = argc > 3 ? atoi(argv[3]) : 405504;
    int poll84 = argc > 4 ? atoi(argv[4]) : 0;
    const unsigned long long frame = 5005312;
    libusb_device **list = NULL;
    libusb_device_handle *h = NULL;
    unsigned char *buf;
    ssize_t n, i;
    int r, found = 0;
    double t0;
    unsigned long long total = 0, last_report = 0;
    long frames = 0;
    int tmo = 0;
    pthread_t th;

    libusb_init(NULL);
    libusb_set_option(NULL, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);

    n = libusb_get_device_list(NULL, &list);
    for (i = 0; i < n; i++)
    {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d))
            continue;
        if (d.idVendor == 0x2a0e && d.idProduct == 0x0080)
        {
            r = libusb_open(list[i], &h);
            printf("open %u:%u -> %d (%s)\n", libusb_get_bus_number(list[i]),
                   libusb_get_device_address(list[i]), r, r ? libusb_error_name(r) : "ok");
            if (r)
                return 1;
            found = 1;
            break;
        }
    }
    libusb_free_device_list(list, 1);
    if (!found)
    {
        printf("device not found\n");
        return 1;
    }
    gh = h;

    r = libusb_reset_device(h);
    printf("reset -> %d (%s)\n", r, r ? libusb_error_name(r) : "ok");
    libusb_set_configuration(h, 1);
    r = libusb_claim_interface(h, 0);
    printf("claim -> %d (%s)\n", r, r ? libusb_error_name(r) : "ok");
    if (r)
        return 1;

    /* Replay the app's full SCPI configuration (captured from a real run). */
    for (int ci = 0; ci < cfg_count; ci++)
    {
        int rr = libusb_control_transfer(h, 0x40, 0xb0, 0, 0, (unsigned char *)cfg[ci].d, cfg[ci].n, 5000);
        if (rr < 0)
        {
            printf("cfg[%d] failed: %d %s\n", ci, rr, libusb_error_name(rr));
            break;
        }
    }
    printf("replayed %d config commands\n", cfg_count);

    /* Recover sequence: the app sends this on stop, but under Wine it stops
     * after the read error and never sends the final 02 04 00. */
    ctl(h, (const unsigned char *)"\x01\x00\x00", 3);
    ctl(h, (const unsigned char *)"\x02\x08\x00", 3);
    ctl(h, (const unsigned char *)"\x01\x00\x00", 3);
    ctl(h, (const unsigned char *)"\x02\x04\x00", 3);
    ctl(h, (const unsigned char *)"\x03\x01\x00", 3);
    ctl(h, (const unsigned char *)"\x02\x00\x00", 3);
    ctl(h, (const unsigned char *)"\x01\x01\x00", 3);
    if (poll84)
        pthread_create(&th, NULL, poll_thread, NULL);

    buf = malloc(chunk);
    t0 = now_s();
    printf("pattern: frame=%llu chunk=%d cap=%.1f MB/s poll84=%d max=%.0fs\n",
           frame, chunk, mbps, poll84, max_s);

    /* header packet first, like the app */
    {
        int xfer = 0;
        r = libusb_bulk_transfer(h, 0x86, buf, 4096, &xfer, 2000);
        printf("header -> %d xfer=%d\n", r, xfer);
        if (!r)
            total += xfer;
    }

    while (running && now_s() - t0 < max_s)
    {
        unsigned long long left = frame;
        while (left && running && now_s() - t0 < max_s)
        {
            int want = left < (unsigned long long)chunk ? (int)left : chunk;
            int xfer = 0;
            r = libusb_bulk_transfer(h, 0x86, buf, want, &xfer, 5000);
            if (r == LIBUSB_ERROR_TIMEOUT)
            {
                if (++tmo > 3) { printf("4 timeouts in a row at total=%llu\n", total); running = 0; break; }
                continue;
            }
            tmo = 0;
            if (r)
            {
                printf("ERROR after %.3fs total=%llu bytes frames=%ld: %d %s xfer=%d\n",
                       now_s() - t0, total, frames, r, libusb_error_name(r), xfer);
                running = 0;
                break;
            }
            total += xfer;
            left -= xfer;
            if (mbps > 0)
            {
                double due = t0 + (double)total / (mbps * 1048576.0);
                double now = now_s();
                if (due > now)
                {
                    struct timespec ts;
                    double d = due - now;
                    ts.tv_sec = (time_t)d;
                    ts.tv_nsec = (long)((d - (double)(time_t)d) * 1e9);
                    nanosleep(&ts, NULL);
                }
            }
            if (total - last_report >= 10ull * 1048576)
            {
                double el = now_s() - t0;
                printf("t=%.2fs total=%.1fMB avg=%.1fMB/s frames=%ld\n", el,
                       total / 1048576.0, total / 1048576.0 / el, frames);
                last_report = total;
            }
        }
        frames++;
    }
    running = 0;
    if (poll84)
        pthread_join(th, NULL);
    printf("done: %.3fs %llu bytes %ld frames\n", now_s() - t0, total, frames);
    if (getenv("DSW_STOP"))
        ctl(h, (const unsigned char *)"\x01\x00\x00", 3);
    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(NULL);
    free(buf);
    return 0;
}
