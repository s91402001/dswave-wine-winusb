/*
 * Linux-side USB helper for the DSWave WinUSB shim.
 *
 * Listens on 127.0.0.1:SHIM_PORT and exposes a tiny libusb-backed RPC that the
 * Windows winusb.dll shim uses.  Run this before starting DSWave under Wine.
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <libusb-1.0/libusb.h>

#include "proto.h"

#define MAX_HANDLES 8
#define VENDOR 0x2a0e

struct conn
{
    int fd;
    unsigned char *out;
    size_t out_cap, out_len;
    int owned[MAX_HANDLES];   /* handles opened by this connection */
};

static libusb_context *ctx;
static pthread_mutex_t usb_lock = PTHREAD_MUTEX_INITIALIZER;
static libusb_device_handle *handles[MAX_HANDLES];
static int handle_refs[MAX_HANDLES];
static int close_pending[MAX_HANDLES];
static volatile uint32_t abort_gen[MAX_HANDLES][256];
static uint64_t stream_off[MAX_HANDLES][256];
static uint64_t last_chunk_us[MAX_HANDLES][256];
static uint64_t last_abort_us[MAX_HANDLES][256];
static uint64_t last_ctl_us;
static unsigned char last_ctl[4];
static int last_ctl_len;

static int dbg(void)
{
    static int on = -1;
    if (on < 0)
    {
        const char *e = getenv("DSW_DEBUG");
        on = (e && *e && *e != '0') ? 1 : 0;
    }
    return on;
}
/* Read-rate cap in MB/s (0 = unlimited).  The device stalls if the host
 * drains its FIFO faster than it can refill it; Windows happens to read at
 * ~19 MB/s, so pacing at or below that keeps the stream alive. */
static uint32_t mbits_cap(void)
{
    static int v = -1;
    if (v < 0)
    {
        const char *e = getenv("DSW_MBPS");
        v = (e && *e) ? atoi(e) : 0;
        if (v < 0)
            v = 0;
    }
    return (uint32_t)v;
}
static pthread_mutex_t ep_lock[MAX_HANDLES][256];

static void init_locks(void)
{
    int i, e;

    for (i = 0; i < MAX_HANDLES; i++)
        for (e = 0; e < 256; e++)
            pthread_mutex_init(&ep_lock[i][e], NULL);
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static uint32_t elapsed_ms(const struct timespec *start)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)((now.tv_sec - start->tv_sec) * 1000 +
                      (now.tv_nsec - start->tv_nsec) / 1000000);
}

/* Keep the handle alive for the duration of a transfer. */
static libusb_device_handle *acquire_handle(uint32_t id)
{
    libusb_device_handle *h = NULL;

    if (id == 0 || id >= MAX_HANDLES)
        return NULL;
    pthread_mutex_lock(&usb_lock);
    if (handles[id] && !close_pending[id])
    {
        h = handles[id];
        handle_refs[id]++;
    }
    pthread_mutex_unlock(&usb_lock);
    return h;
}

static void release_handle(uint32_t id)
{
    libusb_device_handle *h = NULL;

    if (id == 0 || id >= MAX_HANDLES)
        return;
    pthread_mutex_lock(&usb_lock);
    if (handle_refs[id] > 0)
        handle_refs[id]--;
    if (handle_refs[id] == 0 && close_pending[id])
    {
        h = handles[id];
        handles[id] = NULL;
        close_pending[id] = 0;
    }
    pthread_mutex_unlock(&usb_lock);
    if (h)
    {
        libusb_release_interface(h, 0);
        libusb_close(h);
    }
}

static void close_owned(struct conn *c)
{
    int i;

    pthread_mutex_lock(&usb_lock);
    for (i = 1; i < MAX_HANDLES; i++)
    {
        if (!c->owned[i])
            continue;
        c->owned[i] = 0;
        if (!handles[i])
            continue;
        if (handle_refs[i] > 0)
        {
            int e;
            close_pending[i] = 1;
            for (e = 0; e < 256; e++)
                __sync_add_and_fetch(&abort_gen[i][e], 1);
            continue;
        }
        libusb_release_interface(handles[i], 0);
        libusb_close(handles[i]);
        handles[i] = NULL;
    }
    pthread_mutex_unlock(&usb_lock);
}


static int read_full(int fd, void *buf, size_t len)
{
    char *p = buf;
    while (len)
    {
        ssize_t n = read(fd, p, len);
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len)
    {
        ssize_t n = write(fd, p, len);
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int queue_out(struct conn *c, const void *data, size_t len)
{
    if (len > c->out_cap)
    {
        unsigned char *p = realloc(c->out, len);
        if (!p)
            return -1;
        c->out = p;
        c->out_cap = len;
    }
    memcpy(c->out, data, len);
    c->out_len = len;
    return 0;
}

static int handle_for(uint32_t id)
{
    if (id == 0 || id >= MAX_HANDLES || !handles[id])
        return -1;
    return (int)id;
}

static int speed_to_windows(enum libusb_speed speed)
{
    switch (speed)
    {
        case LIBUSB_SPEED_LOW: return 0;
        case LIBUSB_SPEED_FULL: return 1;
        case LIBUSB_SPEED_HIGH: return 2;
        case LIBUSB_SPEED_SUPER: return 3;
        default: return 2;
    }
}

/* Read the raw device and configuration descriptors.  We open the device but
 * deliberately do NOT claim any interface: this is cheap, does not disturb a
 * running capture, and lets the Windows side answer hub IOCTLs without a
 * WinUSB handle. */
static int read_descriptors(libusb_device *dev, struct dev_info *info)
{
    libusb_device_handle *h = NULL;
    unsigned char head[9];
    int ret, total;

    ret = libusb_open(dev, &h);
    if (ret)
        return ret;
    ret = libusb_get_descriptor(h, 0x01, 0, info->device_desc, SHIM_DEV_DESC_LEN);
    if (ret < SHIM_DEV_DESC_LEN)
    {
        libusb_close(h);
        return LIBUSB_ERROR_IO;
    }
    ret = libusb_control_transfer(h, 0x80, 0x06, 0x0200, 0, head, sizeof(head), 2000);
    if (ret >= (int)sizeof(head))
    {
        total = head[2] | (head[3] << 8);
        if (total > SHIM_CONFIG_MAX)
            total = SHIM_CONFIG_MAX;
        ret = libusb_control_transfer(h, 0x80, 0x06, 0x0200, 0, info->config_desc,
                                      (uint16_t)total, 2000);
        if (ret > 0)
            info->config_len = (uint32_t)ret;
    }
    libusb_close(h);
    return 0;
}

/* Descriptor cache.  REQ_LIST runs every ~100 ms while the app is running and
 * used to open the device + issue EP0 GET_DESCRIPTOR each time; during a
 * SuperSpeed bulk stream that froze the FX3 (see progress.md 2026-10-30).
 * Descriptors are now read exactly once per bus:addr. */
#define DESC_CACHE_MAX 8
static struct dev_info desc_cache[DESC_CACHE_MAX];

static struct dev_info *desc_cache_get(uint8_t bus, uint8_t addr, uint16_t vid, uint16_t pid)
{
    int i, slot = -1;

    for (i = 0; i < DESC_CACHE_MAX; i++)
    {
        if (desc_cache[i].vid == vid && desc_cache[i].pid == pid &&
            desc_cache[i].bus == bus && desc_cache[i].addr == addr)
            return &desc_cache[i];
        if (!desc_cache[i].vid && slot < 0)
            slot = i;
    }
    if (slot < 0)
        slot = 0; /* table full: reuse the first slot */
    memset(&desc_cache[slot], 0, sizeof(desc_cache[slot]));
    desc_cache[slot].bus = bus;
    desc_cache[slot].addr = addr;
    desc_cache[slot].vid = vid;
    desc_cache[slot].pid = pid;
    return &desc_cache[slot];
}

static int list_live_mode(void)
{
    static int live = -1;

    if (live < 0)
    {
        const char *e = getenv("DSW_LIST_LIVE");
        live = (e && *e && *e != '0') ? 1 : 0;
    }
    return live;
}

static void do_list(struct conn *c, const struct shim_req *req, struct shim_resp *resp)
{
    libusb_device **list = NULL;
    struct dev_info info[8];
    ssize_t count, i;
    uint32_t n = 0;
    int live;

    (void)req;
    live = list_live_mode();
    pthread_mutex_lock(&usb_lock);
    count = libusb_get_device_list(ctx, &list);
    if (count < 0)
    {
        pthread_mutex_unlock(&usb_lock);
        resp->status = (int32_t)count;
        return;
    }
    for (i = 0; i < count && n < 8; i++)
    {
        struct libusb_device_descriptor desc;
        struct dev_info *cached;
        uint8_t bus, addr;

        if (libusb_get_device_descriptor(list[i], &desc))
            continue;
        if (desc.idVendor != VENDOR)
            continue;
        bus = (uint8_t)libusb_get_bus_number(list[i]);
        addr = (uint8_t)libusb_get_device_address(list[i]);

        cached = desc_cache_get(bus, addr, desc.idVendor, desc.idProduct);
        if (!live && cached->config_len)
        {
            /* Descriptors were read once for this bus:addr; never touch EP0
             * again while a capture may be running. */
            info[n] = *cached;
            info[n].speed = speed_to_windows(libusb_get_device_speed(list[i]));
            info[n].interfaces = desc.bNumConfigurations ? 1 : 0;
        }
        else
        {
            memset(&info[n], 0, sizeof(info[n]));
            info[n].bus = bus;
            info[n].addr = addr;
            info[n].vid = desc.idVendor;
            info[n].pid = desc.idProduct;
            info[n].speed = speed_to_windows(libusb_get_device_speed(list[i]));
            info[n].interfaces = desc.bNumConfigurations ? 1 : 0;
            if (!live)
            {
                struct dev_info fresh;

                memset(&fresh, 0, sizeof(fresh));
                fresh.bus = bus;
                fresh.addr = addr;
                fresh.vid = desc.idVendor;
                fresh.pid = desc.idProduct;
                fresh.speed = info[n].speed;
                fresh.interfaces = info[n].interfaces;
                if (read_descriptors(list[i], &fresh) == 0)
                {
                    *cached = fresh;
                    info[n] = fresh;
                }
            }
        }
        n++;
    }
    libusb_free_device_list(list, 1);
    pthread_mutex_unlock(&usb_lock);
    resp->len = n;
    if (n && queue_out(c, info, n * sizeof(info[0])))
        resp->status = -EIO;
}

static void do_open(struct conn *c, const struct shim_req *req, struct shim_resp *resp)
{
    libusb_device **list = NULL;
    libusb_device *target = NULL, *fallback = NULL;
    struct libusb_device_descriptor tdesc;
    libusb_device_handle *h = NULL;
    ssize_t count, i;
    int ret, id;

    pthread_mutex_lock(&usb_lock);
    count = libusb_get_device_list(ctx, &list);
    if (count < 0)
    {
        pthread_mutex_unlock(&usb_lock);
        resp->status = (int32_t)count;
        return;
    }

    for (i = 0; i < count; i++)
    {
        struct libusb_device_descriptor desc;

        if (libusb_get_device_descriptor(list[i], &desc))
            continue;
        if (desc.idVendor != req->c || desc.idProduct != req->d)
            continue;
        if (!fallback)
            fallback = list[i];
        if ((!req->a || libusb_get_bus_number(list[i]) == req->a) &&
            (!req->b || libusb_get_device_address(list[i]) == req->b))
        {
            target = list[i];
            tdesc = desc;
            break;
        }
    }

    /* The device may have re-enumerated (new address) since the Windows side
     * last scanned; fall back to the only device with our VID/PID. */
    if (!target && fallback)
    {
        target = fallback;
        libusb_get_device_descriptor(target, &tdesc);
        fprintf(stderr, "helper: open fallback %u:%u (requested %u:%u)\n",
                libusb_get_bus_number(target), libusb_get_device_address(target),
                req->a, req->b);
    }
    if (!target)
    {
        libusb_free_device_list(list, 1);
        pthread_mutex_unlock(&usb_lock);
        resp->status = LIBUSB_ERROR_NO_DEVICE;
        return;
    }

    ret = libusb_open(target, &h);
    if (ret)
    {
        libusb_free_device_list(list, 1);
        pthread_mutex_unlock(&usb_lock);
        resp->status = ret;
        return;
    }
    {
        static int did_reset;
        const char *noreset = getenv("DSW_NO_RESET");
        if (!did_reset && !(noreset && *noreset != '0'))
        {
            int rr = libusb_reset_device(h);
            fprintf(stderr, "helper: reset_device (first open) -> %s\n",
                    libusb_error_name(rr));
            did_reset = 1;
        }
    }
    if (libusb_kernel_driver_active(h, 0) == 1)
    {
        ret = libusb_detach_kernel_driver(h, 0);
        if (ret)
            fprintf(stderr, "helper: detach kernel driver: %s\n", libusb_error_name(ret));
    }
    ret = libusb_claim_interface(h, 0);
    if (ret)
    {
        fprintf(stderr, "helper: claim interface 0 (%u:%u): %s\n",
                libusb_get_bus_number(target), libusb_get_device_address(target),
                libusb_error_name(ret));
        libusb_close(h);
        libusb_free_device_list(list, 1);
        pthread_mutex_unlock(&usb_lock);
        resp->status = ret;
        return;
    }

    for (id = 1; id < MAX_HANDLES; id++)
        if (!handles[id])
            break;
    if (id == MAX_HANDLES)
    {
        libusb_release_interface(h, 0);
        libusb_close(h);
        libusb_free_device_list(list, 1);
        pthread_mutex_unlock(&usb_lock);
        resp->status = LIBUSB_ERROR_NO_MEM;
        return;
    }

    /* The device can be left with halted endpoints after a failed capture;
     * clear them on open so a fresh session starts clean. */
    {
        static const uint8_t eps[] = { 0x02, 0x86, 0x84 };
        size_t e;
        for (e = 0; e < sizeof(eps); e++)
        {
            int cr = libusb_clear_halt(h, eps[e]);
            if (cr && cr != LIBUSB_ERROR_NOT_FOUND)
                fprintf(stderr, "helper: clear_halt ep=%02x -> %s\n", eps[e],
                        libusb_error_name(cr));
        }
    }
    handles[id] = h;
    memset(stream_off[id], 0, sizeof(stream_off[id]));
    handle_refs[id] = 0;
    close_pending[id] = 0;
    c->owned[id] = 1;
    resp->handle = (uint32_t)id;
    resp->bus = libusb_get_bus_number(target);
    resp->addr = libusb_get_device_address(target);
    resp->vid = tdesc.idVendor;
    resp->pid = tdesc.idProduct;
    resp->speed = speed_to_windows(libusb_get_device_speed(target));
    resp->status = 0;

    libusb_free_device_list(list, 1);
    pthread_mutex_unlock(&usb_lock);
}

/* Windows' stop sequence: 01 00 00 -> 02 08 00 -> read 0x84 x3 -> 01 00 00 ->
 * 02 04 00.  The app normally sends it, but when a transfer fails it bails out
 * early and the FPGA FIFO stays half-drained; replaying it here returns the
 * device to a clean, restartable state without re-uploading the bitstream. */
static int soft_reset_enabled(void)
{
    static int on = -1;

    if (on < 0)
    {
        const char *e = getenv("DSW_SOFT_RESET");
        /* Off by default: the app opens/closes the device several times while
         * validating it, and replaying the stop sequence there interferes
         * with the FPGA bitstream upload (progress.md 2026-10-30). */
        on = (e && *e && *e != '0') ? 1 : 0;
    }
    return on;
}

static void dsw_hw_soft_reset(libusb_device_handle *h)
{
    unsigned char stop1[3] = {0x01, 0x00, 0x00};
    unsigned char stop2[3] = {0x02, 0x08, 0x00};
    unsigned char stop3[3] = {0x02, 0x04, 0x00};
    unsigned char st = 0;
    unsigned char *trash;
    int i, actual = 0;

    if (!h)
        return;
    libusb_control_transfer(h, 0x40, 0xb0, 0, 0, stop1, 3, 200);
    libusb_control_transfer(h, 0x40, 0xb0, 0, 0, stop2, 3, 200);
    for (i = 0; i < 3; i++)
        libusb_bulk_transfer(h, 0x84, &st, 1, &actual, 50);
    libusb_control_transfer(h, 0x40, 0xb0, 0, 0, stop1, 3, 200);
    libusb_control_transfer(h, 0x40, 0xb0, 0, 0, stop3, 3, 200);
    libusb_clear_halt(h, 0x86);
    trash = malloc(405504);
    if (trash)
    {
        libusb_bulk_transfer(h, 0x86, trash, 405504, &actual, 50);
        free(trash);
    }
    fprintf(stderr, "helper: hw soft reset sent\n");
}

static void do_close(struct conn *c, const struct shim_req *req, struct shim_resp *resp)
{
    int id;

    (void)c;
    pthread_mutex_lock(&usb_lock);
    id = handle_for(req->handle);
    if (id < 0)
    {
        pthread_mutex_unlock(&usb_lock);
        resp->status = LIBUSB_ERROR_INVALID_PARAM;
        return;
    }
    c->owned[id] = 0;
    if (handle_refs[id] > 0)
    {
        int e;
        close_pending[id] = 1;
        for (e = 0; e < 256; e++)
            __sync_add_and_fetch(&abort_gen[id][e], 1);
        pthread_mutex_unlock(&usb_lock);
        resp->status = 0;
        return;
    }
    if (soft_reset_enabled())
        dsw_hw_soft_reset(handles[id]);
    libusb_release_interface(handles[id], 0);
    libusb_close(handles[id]);
    handles[id] = NULL;
    pthread_mutex_unlock(&usb_lock);
    resp->status = 0;
}

static void do_control(struct conn *c, const struct shim_req *req, struct shim_resp *resp)
{
    int id = handle_for(req->handle);
    unsigned char *buf;
    int ret;

    if (id < 0)
    {
        resp->status = LIBUSB_ERROR_INVALID_PARAM;
        return;
    }
    if (req->len > SHIM_MAX_XFER)
    {
        resp->status = LIBUSB_ERROR_INVALID_PARAM;
        return;
    }
    buf = malloc(req->len ? req->len : 1);
    if (!buf)
    {
        resp->status = LIBUSB_ERROR_NO_MEM;
        return;
    }
    if (!(req->a & 0x80) && req->len && read_full(c->fd, buf, req->len))
    {
        free(buf);
        resp->status = -EIO;
        return;
    }
    {
        libusb_device_handle *h = acquire_handle((uint32_t)id);
        if (!h)
        {
            free(buf);
            resp->status = LIBUSB_ERROR_INVALID_PARAM;
            return;
        }
        ret = libusb_control_transfer(h, (uint8_t)req->a, (uint8_t)req->b,
                                      (uint16_t)req->c, (uint16_t)req->d, buf,
                                      (uint16_t)req->len, 5000);
        release_handle((uint32_t)id);
    }
    if (ret < 0)
        resp->status = ret;
    else
    {
        resp->len = (uint32_t)ret;
        if ((req->a & 0x80) && ret && queue_out(c, buf, (size_t)ret))
            resp->status = -EIO;
    }
    if (req->len && req->len <= 4 && !(req->a & 0x80))
    {
        last_ctl_us = now_us();
        memcpy(last_ctl, buf, req->len);
        last_ctl_len = (int)req->len;
    }
    if (dbg())
    {
        if (!(req->a & 0x80) && req->len && req->len <= 16)
        {
            char hex[64];
            uint32_t k, n = req->len < 16 ? req->len : 16;
            for (k = 0; k < n; k++)
                sprintf(hex + k * 3, "%02x ", buf[k]);
            fprintf(stderr, "helper: ctl OUT bm=%02x br=%02x wv=%04x wi=%04x len=%u ret=%d data=%s\n",
                    req->a, req->b, req->c, req->d, req->len, ret, hex);
        }
        else
            fprintf(stderr, "helper: ctl %s bm=%02x br=%02x wv=%04x wi=%04x len=%u ret=%d\n",
                    (req->a & 0x80) ? "IN " : "OUT", req->a, req->b, req->c, req->d,
                    req->len, ret);
    }
    /* Only the FIFO-reset command (b0 02 04 00) aborts pending reads.  The
     * 01 00 00 / 02 08 00 pair is also used for a mid-capture "flow pause"
     * (settings change), so aborting there kills a healthy stream.  For Stop,
     * the app calls libusb_cancel_transfer first, which our CancelIoEx hook
     * completes immediately. */
    if (ret >= 0 && req->a == 0x40 && req->b == 0xb0 && req->len == 3 &&
        buf[0] == 0x02 && buf[1] == 0x04)
    {
        int e;
        for (e = 0x81; e <= 0xff; e++)
        {
            if (!(e & 0x80))
                continue;
            last_abort_us[id][e] = now_us();
            __sync_add_and_fetch(&abort_gen[id][e], 1);
        }
        fprintf(stderr, "helper: fifo reset cmd -> abort pending IN (id=%d)\n", id);
    }
    free(buf);
}

static int fpga_recover_enabled(void)
{
    static int on = -1;

    if (on < 0)
    {
        const char *e = getenv("DSW_FPGA_RECOVER");
        on = (e && *e == '0') ? 0 : 1; /* on by default */
    }
    return on;
}

/* Vendor status read c0 b2, 2 bytes; byte0 03 = idle/unconfigured, 0x80 bit =
 * configured. Returns byte0 or a negative libusb error. */
static int fpga_b2(libusb_device_handle *h)
{
    unsigned char b[2] = {0, 0};
    int r = libusb_control_transfer(h, 0xC0, 0xB2, 0, 0, b, 2, 5000);
    return r == 2 ? b[0] : r;
}

/* Replay the pre-upload sequence just before the app's bitstream OUT.
 * dswfpga v3 (tools/) pinned the exact recipe: Windows always reads b2 ==
 * 03 00 (idle, unconfigured) right before the PROG pulse, spacing commands
 * ~15 ms apart. Uploading while the device is still busy (b2 == 01) leaves
 * the FPGA unconfigured. The zero-length IN seen in the pcap is host-side
 * only (the device has no EP 0x82), so it is not replayed here.
 * Returns 0 when the device may be programmed (or is already configured),
 * or a negative libusb error if the attempt must be aborted. The USB reset
 * is kept because it also recovers devices that are wedged from an earlier
 * session; it re-enumerates the device, which the helper's live REQ_LIST
 * cache absorbs. */
static int fpga_prepare(libusb_device_handle *h)
{
    /* pre-PROG commands (PROG + init are issued after the idle wait) */
    static const struct { uint8_t req; uint8_t d[3]; } pre[] = {
        {0xb1, {0xf0, 0x02, 0x00}}, {0xb1, {0xf2, 0x02, 0x00}},
        {0xb1, {0xff, 0x02, 0x00}}, {0xb0, {0xfe, 0xfe, 0xff}},
        {0xb0, {0xfe, 0x01, 0x00}}, {0xb1, {0xff, 0x02, 0x00}},
    };
    static const struct { uint8_t req; uint8_t d[3]; } prog[] = {
        {0xb0, {0xfc, 0xe8, 0x1d}}, {0xb0, {0xfd, 0x07, 0x00}},
    };
    struct timespec ts = {0, 15 * 1000 * 1000}; /* 15 ms */
    struct timespec one = {1, 0};
    size_t i;
    int r;

    r = fpga_b2(h);
    if (r >= 0 && (r & 0x80))
    {
        fprintf(stderr, "helper: fpga prepare: already configured (b2=%02x), skip\n", r);
        return 0;
    }

    libusb_release_interface(h, 0);
    r = libusb_reset_device(h);
    fprintf(stderr, "helper: fpga prepare: reset -> %s\n", libusb_error_name(r));
    libusb_set_configuration(h, 1);
    r = libusb_claim_interface(h, 0);
    fprintf(stderr, "helper: fpga prepare: claim -> %s\n", libusb_error_name(r));
    if (r < 0)
        return r;

    for (i = 0; i < sizeof(pre) / sizeof(pre[0]); i++)
    {
        if (dbg())
            fprintf(stderr, "helper: fpga pre: %02x %02x %02x %02x\n", pre[i].req,
                    pre[i].d[0], pre[i].d[1], pre[i].d[2]);
        libusb_control_transfer(h, 0x40, pre[i].req, 0, 0,
                                (unsigned char *)pre[i].d, 3, 2000);
        nanosleep(&ts, NULL);
    }
    /* hard precondition: never pulse PROG unless the device reports idle */
    for (i = 0; i < 10; i++)
    {
        r = fpga_b2(h);
        if (r == 0x03)
            break;
        fprintf(stderr, "helper: fpga prepare: not idle yet b2=%d, waiting\n", r);
        nanosleep(&one, NULL);
    }
    if (r != 0x03)
    {
        fprintf(stderr, "helper: fpga prepare: idle wait failed (b2=%d), refusing PROG\n", r);
        return LIBUSB_ERROR_BUSY;
    }
    for (i = 0; i < sizeof(prog) / sizeof(prog[0]); i++)
    {
        if (dbg())
            fprintf(stderr, "helper: fpga prog: %02x %02x %02x %02x\n", prog[i].req,
                    prog[i].d[0], prog[i].d[1], prog[i].d[2]);
        libusb_control_transfer(h, 0x40, prog[i].req, 0, 0,
                                (unsigned char *)prog[i].d, 3, 2000);
        nanosleep(&ts, NULL);
    }
    return 0;
}

struct bulk_wait
{
    volatile int done;
    int status;
    int actual;
};

static void LIBUSB_CALL bulk_done(struct libusb_transfer *xfer)
{
    struct bulk_wait *w = xfer->user_data;

    w->status = xfer->status;
    w->actual = xfer->actual_length;
    w->done = 1;
}

static void do_bulk(struct conn *c, const struct shim_req *req, struct shim_resp *resp)
{
    uint32_t id = req->handle;
    uint8_t ep = (uint8_t)req->a;
    libusb_device_handle *h;
    unsigned char *buf;
    uint32_t total = 0, my_gen, slices = 0, timeouts = 0;
    uint32_t cap = mbits_cap();
    uint64_t pace_start_us = 0;
    int retried = 0;
    const char *why = "start";
    uint32_t timeout_ms = req->b ? req->b : 300000;
    uint32_t seq = req->d;
    int ret = 0;
    int is_in = (ep & 0x80) != 0;
    struct timespec start;

    if (id == 0 || id >= MAX_HANDLES || ep == 0)
    {
        resp->status = LIBUSB_ERROR_INVALID_PARAM;
        return;
    }
    if (req->len > SHIM_MAX_XFER)
    {
        resp->status = LIBUSB_ERROR_INVALID_PARAM;
        return;
    }
    if (timeout_ms > 300000)
        timeout_ms = 300000;

    buf = malloc(req->len ? req->len : 1);
    if (!buf)
    {
        resp->status = LIBUSB_ERROR_NO_MEM;
        return;
    }
    if (!is_in && req->len && read_full(c->fd, buf, req->len))
    {
        free(buf);
        resp->status = -EIO;
        return;
    }
    if (!is_in && ep == 0x02 && req->len > 100000)
    {
        /* FPGA bitstream upload: keep an up-to-date copy so it can be
         * replayed from Linux without any Windows. Overwrite every time so
         * the file never goes stale after an application/bitstream upgrade. */
        FILE *f = fopen("/tmp/dswave-fpga.bin", "wb");
        if (f)
        {
            size_t wrote = fwrite(buf, 1, req->len, f);
            fclose(f);
            fprintf(stderr, "helper: FPGA bitstream %u bytes dumped to "
                    "/tmp/dswave-fpga.bin (%zu written)\n", req->len, wrote);
        }
    }

    if (dbg())
        fprintf(stderr, "helper: bulk start seq=%u ep=%02x len=%u timeout=%u\n", seq, ep,
                req->len, timeout_ms);
    /* Transfers on the same endpoint are serialised to keep the ring in FIFO
     * order; different endpoints run fully in parallel. */
    /* Snapshot the abort generation BEFORE waiting for the endpoint lock:
     * a queued transfer (e.g. the second 5 MB read) would otherwise adopt the
     * post-abort value and keep submitting URBs to a stopped device. */
    my_gen = __sync_fetch_and_add(&abort_gen[id][ep], 0);
    pthread_mutex_lock(&ep_lock[id][ep]);
    if (__sync_fetch_and_add(&abort_gen[id][ep], 0) != my_gen || close_pending[id])
    {
        pthread_mutex_unlock(&ep_lock[id][ep]);
        free(buf);
        resp->status = LIBUSB_ERROR_INTERRUPTED;
        fprintf(stderr, "helper: abort while queued ep=%02x seq=%u (id=%u)\n", ep, seq, id);
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &start);

    h = acquire_handle(id);
    if (!h)
    {
        pthread_mutex_unlock(&ep_lock[id][ep]);
        free(buf);
        resp->status = LIBUSB_ERROR_INVALID_PARAM;
        return;
    }
    if (fpga_recover_enabled() && !is_in && ep == 0x02 && req->len > 100000)
    {
        int pr = fpga_prepare(h);
        if (pr < 0)
        {
            pthread_mutex_unlock(&ep_lock[id][ep]);
            free(buf);
            resp->status = pr;
            fprintf(stderr, "helper: fpga prepare failed (%s), refusing bulk OUT\n",
                    libusb_error_name(pr));
            return;
        }
    }

    {
        static uint32_t chunk;
        static int fill_mode = -1;
        uint32_t xchunk;

        if (!chunk)
        {
            const char *env = getenv("DSW_CHUNK_BYTES");
            /* 405,504 matches the transfer size Windows/WinUSB uses. */
            chunk = env ? (uint32_t)strtoul(env, NULL, 10) : 405504;
        }
        if (fill_mode < 0)
        {
            const char *env = getenv("DSW_FILL");
            /* Fill the caller's whole buffer by chaining device packets. */
            fill_mode = (env && *env == '0') ? 0 : 1;
        }
        /* OUT transfers must never be split: the FX3 FPGA loader requires the
         * whole bitstream in one transfer (a 405,504-byte first chunk fails
         * with LIBUSB_ERROR_IO, see progress.md 2026-10-30). */
        xchunk = is_in ? chunk : req->len;

        while ((int)(req->len - total) > 0)
        {
            int remain = (int)(req->len - total);
            struct bulk_wait w;
            struct libusb_transfer *xfer;
            int submitted, cancelled = 0;
            uint64_t t0, gap;

            if (xchunk && (uint32_t)remain > xchunk)
                remain = (int)xchunk;

            memset(&w, 0, sizeof(w));
            xfer = libusb_alloc_transfer(0);
            if (!xfer)
            {
                ret = LIBUSB_ERROR_NO_MEM;
                why = "nomem";
                break;
            }
            libusb_fill_bulk_transfer(xfer, h, ep, buf + total, remain, bulk_done, &w,
                                      timeout_ms);
            submitted = libusb_submit_transfer(xfer);
            if (submitted)
            {
                libusb_free_transfer(xfer);
                ret = submitted;
                why = "submit";
                break;
            }
            t0 = now_us();
            gap = last_chunk_us[id][ep] ? t0 - last_chunk_us[id][ep] : 0;

            /* Keep the URB in flight for the caller's full timeout; only the
             * app's abort (WinUsb_AbortPipe / stop) cancels it.  The local
             * 50 ms tick is a poll, not a URB timeout, so no URB is discarded
             * while the device is merely idle. */
            while (!w.done)
            {
                struct timeval tv;

                if (!cancelled &&
                    __sync_fetch_and_add(&abort_gen[id][ep], 0) != my_gen)
                {
                    libusb_cancel_transfer(xfer);
                    cancelled = 1;
                }
                tv.tv_sec = 0;
                tv.tv_usec = 50000;
                libusb_handle_events_timeout_completed(ctx, &tv, NULL);
                if (!w.done)
                    usleep(200);
            }
            /* The callback has run: it is now safe to free the transfer. */
            last_chunk_us[id][ep] = now_us();
            slices++;

            ret = 0;
            switch (w.status)
            {
                case LIBUSB_TRANSFER_COMPLETED:
                    ret = 0;
                    break;
                case LIBUSB_TRANSFER_TIMED_OUT:
                    ret = LIBUSB_ERROR_TIMEOUT;
                    timeouts++;
                    break;
                case LIBUSB_TRANSFER_CANCELLED:
                    ret = LIBUSB_ERROR_INTERRUPTED;
                    break;
                case LIBUSB_TRANSFER_STALL:
                    ret = LIBUSB_ERROR_PIPE;
                    break;
                case LIBUSB_TRANSFER_NO_DEVICE:
                    ret = LIBUSB_ERROR_NO_DEVICE;
                    break;
                case LIBUSB_TRANSFER_OVERFLOW:
                    ret = LIBUSB_ERROR_OVERFLOW;
                    break;
                default:
                    ret = LIBUSB_ERROR_IO;
                    break;
            }
            libusb_free_transfer(xfer);
            xfer = NULL;

            if (w.actual > 0)
            {
                total += (uint32_t)w.actual;
                if (!pace_start_us)
                    pace_start_us = t0;
            }
            if (cap && is_in && pace_start_us && total > 0)
            {
                uint64_t due = pace_start_us +
                    (uint64_t)((double)total * 1e6 / (cap * 1048576.0));
                uint64_t now = now_us();
                if (due > now)
                {
                    struct timespec ts;
                    ts.tv_sec = (time_t)((due - now) / 1000000);
                    ts.tv_nsec = (long)(((due - now) % 1000000) * 1000);
                    nanosleep(&ts, NULL);
                }
            }
            if (dbg() || ret < 0)
                fprintf(stderr, "helper: chunk seq=%u n=%u xfer=%d ret=%d total=%u/%u gap_us=%llu\n",
                        seq, slices, w.actual, ret, total, req->len, (unsigned long long)gap);
            if (total >= 8 && buf[0] == 0x55 && buf[1] == 0x55 && buf[2] == 0x55 && buf[3] == 0x55)
                fprintf(stderr, "helper: header type=%u len=%u total=%u seq=%u\n",
                        buf[4], req->len, total, seq);

            if (ret == 0)
            {
                why = w.actual ? "short" : "zlp";
                if (fill_mode && total < (uint32_t)req->len && w.actual > 0)
                    continue; /* keep filling the caller's buffer */
                break;        /* short packet / ZLP completes the transfer */
            }
            if (ret == LIBUSB_ERROR_PIPE || ret == LIBUSB_ERROR_IO)
            {
                uint64_t since_abort = last_abort_us[id][ep] ?
                    (now_us() - last_abort_us[id][ep]) : 0;
                fprintf(stderr, "helper: stall/io seq=%u ep=%02x ret=%d xfer=%d "
                        "since_abort_us=%llu since_ctl_us=%llu last_ctl=%02x%02x%02x\n",
                        seq, ep, ret, w.actual, (unsigned long long)since_abort,
                        (unsigned long long)(last_ctl_us ? now_us() - last_ctl_us : 0),
                        last_ctl[0], last_ctl[1], last_ctl[2]);
                if (__sync_fetch_and_add(&abort_gen[id][ep], 0) != my_gen)
                {
                    ret = LIBUSB_ERROR_INTERRUPTED;
                    why = "abort";
                    break;
                }
                if (!retried && w.actual == 0 && total == 0)
                {
                    retried = 1;
                    fprintf(stderr, "helper: retry-after-stall ep=%02x\n", ep);
                    libusb_clear_halt(h, ep);
                    continue;
                }
                /* Mid-transfer stall: the device stopped streaming.  Return
                 * the partial data and the error right away, like WinUSB. */
                libusb_clear_halt(h, ep);
                why = "stall";
                break;
            }
            if (ret != LIBUSB_ERROR_TIMEOUT)
            {
                why = "error";
                break;
            }
            why = "timeout";
            break; /* the full caller timeout elapsed */
        }
    }

    if ((ret == LIBUSB_ERROR_IO || ret == LIBUSB_ERROR_PIPE) && total == 0)
    {
        fprintf(stderr, "helper: bulk ep=%02x failed (%s), clearing halt\n", ep,
                libusb_error_name(ret));
        libusb_clear_halt(h, ep);
    }

    release_handle(id);
    pthread_mutex_unlock(&ep_lock[id][ep]);

    if (total > 0 && is_in && dbg())
    {
        char hex[256];
        uint32_t k, n = total < 64 ? total : 64;
        uint64_t base = stream_off[id][ep];

        for (k = 0; k < n; k++)
            sprintf(hex + k * 3, "%02x ", buf[k]);
        fprintf(stderr, "helper: bulk data seq=%u ep=%02x len=%u off=%llu head64=%s\n",
                seq, ep, total, (unsigned long long)base, hex);

        if (total <= 8192)
        {
            char tail[256];
            uint32_t t0 = total > 64 ? total - 64 : 0;
            for (k = t0; k < total; k++)
                sprintf(tail + (k - t0) * 3, "%02x ", buf[k]);
            fprintf(stderr, "helper: bulk tail seq=%u ep=%02x off=%llu tail64=%s\n",
                    seq, ep, (unsigned long long)(base + t0), tail);
            /* list non-zero 32-bit words in the whole small packet */
            {
                uint32_t w, hits = 0;
                for (w = 0; w + 4 <= total && hits < 24; w += 4)
                {
                    uint32_t v;
                    memcpy(&v, buf + w, 4);
                    if (v)
                    {
                        fprintf(stderr, "helper: word seq=%u off=%u val=%08x\n", seq, w, v);
                        hits++;
                    }
                }
            }
        }

        /* locate the 0x55555555 sync word inside this transfer */
        {
            uint32_t found = 0;
            for (k = 0; k + 4 <= total && found < 4; k++)
            {
                if (buf[k] == 0x55 && buf[k+1] == 0x55 && buf[k+2] == 0x55 && buf[k+3] == 0x55)
                {
                    fprintf(stderr, "helper: magic seq=%u ep=%02x rel=%u abs=%llu\n",
                            seq, ep, k, (unsigned long long)(base + k));
                    found++;
                    k += 3;
                }
            }
        }
    }
    /* Deliver partial data and the real status.  Do not mask a mid-transfer
     * error as success: the Windows side must see the failure (the partial
     * bytes are still handed to the caller). */
    /* Report the transferred byte count for OUT transfers too: the app checks
     * the number of bytes written for the FPGA bitstream (a 0-byte "success"
     * makes it treat the upload as failed). */
    if (total > 0)
    {
        resp->len = total;
        if (is_in && queue_out(c, buf, total))
            resp->status = -EIO;
    }
    if (resp->status == 0 && ret < 0)
        resp->status = ret;
    if (is_in && total > 0)
        stream_off[id][ep] += total;
    fprintf(stderr, "helper: bulk done t=%.3f seq=%u ep=%02x status=%d total=%u slices=%u "
                    "timeouts=%u why=%s\n", now_us() / 1e6, seq, ep, resp->status, total, slices,
                    timeouts, why);
    free(buf);
}

static void do_descriptor(struct conn *c, const struct shim_req *req, struct shim_resp *resp)
{
    int id = handle_for(req->handle);
    unsigned char *buf;
    int ret;

    if (id < 0)
    {
        resp->status = LIBUSB_ERROR_INVALID_PARAM;
        return;
    }
    if (!req->len || req->len > SHIM_MAX_XFER)
    {
        resp->status = LIBUSB_ERROR_INVALID_PARAM;
        return;
    }
    buf = malloc(req->len);
    if (!buf)
    {
        resp->status = LIBUSB_ERROR_NO_MEM;
        return;
    }
    {
        libusb_device_handle *h = acquire_handle((uint32_t)id);
        if (!h)
        {
            free(buf);
            resp->status = LIBUSB_ERROR_INVALID_PARAM;
            return;
        }
        ret = libusb_control_transfer(h, 0x80, 0x06,
                                      (uint16_t)((req->a << 8) | req->b),
                                      (uint16_t)req->c, buf, (uint16_t)req->len, 5000);
        release_handle((uint32_t)id);
    }
    if (ret < 0)
        resp->status = ret;
    else
    {
        resp->len = (uint32_t)ret;
        if (ret && queue_out(c, buf, (size_t)ret))
            resp->status = -EIO;
    }
    free(buf);
}

static void do_abort_pipe(const struct shim_req *req, struct shim_resp *resp)
{
    uint32_t id = req->handle;
    uint8_t ep = (uint8_t)req->a;

    if (id == 0 || id >= MAX_HANDLES)
    {
        resp->status = LIBUSB_ERROR_INVALID_PARAM;
        return;
    }
    last_abort_us[id][ep] = now_us();
    __sync_add_and_fetch(&abort_gen[id][ep], 1);
    fprintf(stderr, "helper: abort ep=%02x gen=%u\n", ep, abort_gen[id][ep]);
    resp->status = 0;
}

static void do_reset_pipe(const struct shim_req *req, struct shim_resp *resp)
{
    uint32_t id = req->handle;
    uint8_t ep = (uint8_t)req->a;
    libusb_device_handle *h = acquire_handle(id);
    int ret;

    if (!h)
    {
        resp->status = LIBUSB_ERROR_INVALID_PARAM;
        return;
    }
    ret = libusb_clear_halt(h, ep);
    release_handle(id);
    fprintf(stderr, "helper: reset ep=%02x -> %s\n", ep, libusb_error_name(ret));
    resp->status = ret;
}

static void serve(struct conn *c)
{
    for (;;)
    {
        struct shim_req req;
        struct shim_resp resp;

        if (read_full(c->fd, &req, sizeof(req)))
            return;
        memset(&resp, 0, sizeof(resp));
        c->out_len = 0;
        switch (req.type)
        {
            case REQ_LIST: do_list(c, &req, &resp); break;
            case REQ_OPEN: do_open(c, &req, &resp); break;
            case REQ_CLOSE: do_close(c, &req, &resp); break;
            case REQ_CONTROL: do_control(c, &req, &resp); break;
            case REQ_BULK: do_bulk(c, &req, &resp); break;
            case REQ_DESCRIPTOR: do_descriptor(c, &req, &resp); break;
            case REQ_ABORT_PIPE: do_abort_pipe(&req, &resp); break;
            case REQ_RESET_PIPE: do_reset_pipe(&req, &resp); break;
            default: resp.status = -EINVAL; break;
        }
        if (dbg() || resp.status)
            fprintf(stderr, "helper: req=%u handle=%u a=%02x b=%02x c=%04x d=%04x len=%u"
                            " -> status=%d len=%u\n",
                    req.type, req.handle, req.a, req.b, req.c, req.d, req.len,
                    resp.status, resp.len);
        if (write_full(c->fd, &resp, sizeof(resp)))
            return;
        if (c->out_len && write_full(c->fd, c->out, c->out_len))
            return;
    }
}

static void *connection_thread(void *arg)
{
    struct conn *c = arg;
    serve(c);
    close_owned(c);
    close(c->fd);
    free(c->out);
    free(c);
    return NULL;
}

static int self_test(void)
{
    libusb_device **list = NULL;
    ssize_t count, i;
    int ret;

    if (libusb_init(&ctx))
    {
        fprintf(stderr, "selftest: libusb_init failed\n");
        return 1;
    }
    count = libusb_get_device_list(ctx, &list);
    printf("selftest: %zd USB devices\n", count);
    for (i = 0; i < count; i++)
    {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc))
            continue;
        if (desc.idVendor != VENDOR)
            continue;
        printf("selftest: found %04x:%04x at %u:%u speed %d\n", desc.idVendor,
               desc.idProduct, libusb_get_bus_number(list[i]),
               libusb_get_device_address(list[i]),
               speed_to_windows(libusb_get_device_speed(list[i])));
        {
            libusb_device_handle *h = NULL;
            unsigned char buf[18];
            ret = libusb_open(list[i], &h);
            printf("selftest: open -> %d (%s)\n", ret, ret ? libusb_error_name(ret) : "ok");
            if (!ret)
            {
                ret = libusb_get_descriptor(h, 1, 0, buf, sizeof(buf));
                printf("selftest: device descriptor -> %d bytes (bcdUSB=%02x%02x)\n",
                       ret, buf[3], buf[2]);
                libusb_close(h);
            }
        }
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return 0;
}

int main(int argc, char **argv)
{
    int listen_fd, one = 1;
    struct sockaddr_in addr;

    signal(SIGPIPE, SIG_IGN);
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return self_test();

    if (libusb_init(&ctx))
    {
        fprintf(stderr, "helper: libusb_init failed\n");
        return 1;
    }
    if (dbg())
        libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_DEBUG);
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {
        perror("socket");
        return 1;
    }
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(SHIM_PORT);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)))
    {
        perror("bind");
        return 1;
    }
    init_locks();
    listen(listen_fd, 8);
    fprintf(stderr, "helper: listening on 127.0.0.1:%d\n", SHIM_PORT);

    for (;;)
    {
        struct conn *c;
        pthread_t thread;
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0)
        {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        c = calloc(1, sizeof(*c));
        if (!c)
        {
            close(fd);
            continue;
        }
        c->fd = fd;
        if (pthread_create(&thread, NULL, connection_thread, c))
        {
            close(fd);
            free(c);
            continue;
        }
        pthread_detach(thread);
    }
    libusb_exit(ctx);
    return 0;
}
