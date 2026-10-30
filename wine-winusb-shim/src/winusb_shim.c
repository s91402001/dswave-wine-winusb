/*
 * winusb.dll replacement for DSWave under Wine.
 *
 * Control transfers run synchronously on one persistent helper connection.
 * Bulk transfers use real OVERLAPPED semantics: the call returns
 * ERROR_IO_PENDING immediately and a worker thread (with its own TCP
 * connection) completes the OVERLAPPED later.  This lets libusb keep a ring of
 * transfers in flight while control traffic continues in parallel.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winusb.h>
#include <usbiodef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "proto.h"
#include "shim_internal.h"

#define MAX_CTX 8
#define VENDOR_ID 0x2a0e
#define PRODUCT_ID 0x0080

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS 0x00000000UL
#endif
#ifndef STATUS_PENDING
#define STATUS_PENDING 0x00000103UL
#endif
#ifndef STATUS_CANCELLED
#define STATUS_CANCELLED 0xC0000120UL
#endif
#ifndef STATUS_DEVICE_DATA_ERROR
#define STATUS_DEVICE_DATA_ERROR 0xC000009CUL
#endif
#ifndef STATUS_IO_TIMEOUT
#define STATUS_IO_TIMEOUT 0xC00000B5UL
#endif
#ifndef STATUS_UNSUCCESSFUL
#define STATUS_UNSUCCESSFUL 0xC0000001UL
#endif

#define BULK_WORKERS 8
#define BULK_QUEUE_MAX 128

struct pipe_desc
{
    uint8_t addr;
    uint8_t type;
    uint16_t maxpkt;
    uint8_t interval;
};

struct ctx
{
    int used;
    uint32_t handle;
    uint32_t speed;
    uint32_t bus, addr;
    unsigned char config[4096];
    uint32_t config_len;
    struct pipe_desc pipes[16];
    uint32_t num_pipes;
    uint32_t timeout_ms;
    HANDLE file;
    uint32_t seq_next[256];
};

struct bulk_ctx
{
    struct ctx *dev;
    uint32_t remote;
    UCHAR pipe;
    PUCHAR buffer;
    ULONG length;
    ULONG timeout;
    int is_in;
    uint32_t seq;
    LPOVERLAPPED overlapped;
    volatile LONG cancelled;
    int completed;
    struct bulk_ctx *next;
};

static struct ctx contexts[MAX_CTX];
static CRITICAL_SECTION lock;          /* protects the control socket */
static CRITICAL_SECTION q_lock;
static CRITICAL_SECTION active_lock;
static CRITICAL_SECTION log_lock;
static SOCKET sock = INVALID_SOCKET;
static LONG initialized;

static struct bulk_ctx *queue[BULK_QUEUE_MAX];
static int q_head, q_tail, q_count;
static HANDLE q_sem;
static HANDLE workers[BULK_WORKERS];
static SOCKET worker_sock[BULK_WORKERS];
static struct bulk_ctx *active_list;

typedef int (*iocp_lookup_t)(HANDLE, HANDLE *, ULONG_PTR *);

/* The I/O completion port map lives in the setupapi shim, which installs the
 * CreateIoCompletionPort hook before any transfer happens. */
static int iocp_lookup(HANDLE file, HANDLE *iocp, ULONG_PTR *key)
{
    static iocp_lookup_t fn;
    HMODULE m;

    if (!fn)
    {
        m = GetModuleHandleA("setupapi.dll");
        if (!m)
            m = LoadLibraryA("setupapi.dll");
        if (m)
            fn = (iocp_lookup_t)GetProcAddress(m, "shim_lookup_iocp");
    }
    return fn ? fn(file, iocp, key) : 0;
}

static void shim_log(const char *fmt, ...)
{
    char buf[512];
    va_list args;
    FILE *f;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    /* Several worker threads log concurrently; a plain fopen("a") can fail
     * with a sharing violation and silently drop lines. */
    EnterCriticalSection(&log_lock);
    f = fopen("C:\\ProgramData\\DSWaveShim\\winusb.log", "a");
    if (f)
    {
        fprintf(f, "%s\n", buf);
        fclose(f);
    }
    LeaveCriticalSection(&log_lock);
}

static SOCKET open_helper_socket(void)
{
    SOCKET s;
    struct sockaddr_in addr;
    DWORD tv = 310000; /* larger than the maximum bulk timeout */

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET)
        return INVALID_SOCKET;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&(int){1}, sizeof(int));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SHIM_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

static int send_all(SOCKET s, const void *data, size_t len)
{
    const char *p = data;
    while (len)
    {
        int n = send(s, p, (int)len, 0);
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int recv_all(SOCKET s, void *data, size_t len)
{
    char *p = data;
    while (len)
    {
        int n = recv(s, p, (int)len, 0);
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int rpc_on(SOCKET s, const struct shim_req *req, struct shim_resp *resp,
                  void *payload, uint32_t payload_len, int expect_reply)
{
    if (send_all(s, req, sizeof(*req)) ||
        (payload_len && send_all(s, payload, payload_len)))
        return -1;
    if (recv_all(s, resp, sizeof(*resp)))
        return -1;
    if (expect_reply && resp->len && recv_all(s, payload, resp->len))
        return -1;
    return 0;
}

/* control socket, serialised */
static int rpc(const struct shim_req *req, struct shim_resp *resp, void *payload,
               uint32_t payload_len, int expect_reply)
{
    int ret;

    if (sock == INVALID_SOCKET)
    {
        sock = open_helper_socket();
        if (sock == INVALID_SOCKET)
        {
            SetLastError(ERROR_DEVICE_NOT_CONNECTED);
            return -1;
        }
    }
    ret = rpc_on(sock, req, resp, payload, payload_len, expect_reply);
    if (ret)
    {
        closesocket(sock);
        sock = INVALID_SOCKET;
        SetLastError(ERROR_DEVICE_NOT_CONNECTED);
    }
    return ret;
}

static struct ctx *ctx_from(WINUSB_INTERFACE_HANDLE h)
{
    struct ctx *c = (struct ctx *)h;
    if (!c || c < contexts || c >= contexts + MAX_CTX || !c->used)
        return NULL;
    return c;
}

static struct ctx *ctx_alloc(void)
{
    int i;
    for (i = 0; i < MAX_CTX; i++)
        if (!contexts[i].used)
            return &contexts[i];
    return NULL;
}

static int parse_device_path(HANDLE file, unsigned *bus, unsigned *addr)
{
    WCHAR path[1024];
    DWORD len;
    const WCHAR *name;

    len = GetFinalPathNameByHandleW(file, path, ARRAYSIZE(path), FILE_NAME_NORMALIZED);
    if (!len || len >= ARRAYSIZE(path))
        return -1;
    name = wcsrchr(path, L'\\');
    if (!name)
        return -1;
    name++;
    if (swscanf(name, L"usb-%u-%u.dev", bus, addr) == 2)
        return 0;
    if (swscanf(name, L"USB#VID_%*4x&PID_%*4x#%u&%u", bus, addr) == 2)
        return 0;
    return -1;
}

static void parse_config(struct ctx *c)
{
    const unsigned char *p = c->config;
    uint32_t left = c->config_len;
    int in_iface = 0;

    while (left >= 2 && p[0] >= 2 && p[0] <= left)
    {
        if (p[1] == 0x04 && p[0] >= 9)
            in_iface = (p[2] == 0 && p[3] == 0);
        else if (p[1] == 0x05 && p[0] >= 7 && in_iface && c->num_pipes < 16)
        {
            c->pipes[c->num_pipes].addr = p[2];
            c->pipes[c->num_pipes].type = p[3] & 3;
            c->pipes[c->num_pipes].maxpkt = (uint16_t)(p[4] | (p[5] << 8));
            c->pipes[c->num_pipes].interval = p[6];
            c->num_pipes++;
        }
        left -= p[0];
        p += p[0];
    }
}

/* ------------------------------------------------------------------ */
/* worker pool                                                         */

static void active_add(struct bulk_ctx *b)
{
    EnterCriticalSection(&active_lock);
    b->next = active_list;
    active_list = b;
    LeaveCriticalSection(&active_lock);
}

static void active_remove(struct bulk_ctx *b)
{
    struct bulk_ctx **p;

    EnterCriticalSection(&active_lock);
    for (p = &active_list; *p; p = &(*p)->next)
    {
        if (*p == b)
        {
            *p = b->next;
            break;
        }
    }
    LeaveCriticalSection(&active_lock);
}

static void finish_bulk(struct bulk_ctx *b, ULONG status, ULONG actual)
{
    LPOVERLAPPED ov = b->overlapped;
    struct bulk_ctx **p;
    int do_post;

    EnterCriticalSection(&active_lock);
    do_post = !b->completed;
    if (do_post)
        b->completed = 1;
    for (p = &active_list; *p; p = &(*p)->next)
    {
        if (*p == b)
        {
            *p = b->next;
            break;
        }
    }
    LeaveCriticalSection(&active_lock);

    if (do_post)
    {
        HANDLE iocp;
        ULONG_PTR key;

        ov->InternalHigh = actual;
        ov->Internal = status;
        MemoryBarrier();
        if (iocp_lookup(b->dev->file, &iocp, &key))
            PostQueuedCompletionStatus(iocp, (DWORD)actual, key, ov);
        else if (ov->hEvent)
            SetEvent(ov->hEvent);
    }
    free(b);
}

static void run_bulk_worker(struct bulk_ctx *b, int index)
{
    struct shim_req req;
    struct shim_resp resp;
    SOCKET s = worker_sock[index];
    int ok;

    if (b->cancelled)
    {
        shim_log("worker post seq=%u bytes=0 status=CANCELLED", b->seq);
        finish_bulk(b, STATUS_CANCELLED, 0);
        return;
    }

    if (s == INVALID_SOCKET)
    {
        s = open_helper_socket();
        worker_sock[index] = s;
    }
    if (s == INVALID_SOCKET)
    {
        finish_bulk(b, STATUS_UNSUCCESSFUL, 0);
        return;
    }

    memset(&req, 0, sizeof(req));
    req.type = REQ_BULK;
    req.handle = b->remote;
    req.a = b->pipe;
    req.b = b->timeout;
    req.d = b->seq;
    req.len = b->length;

    shim_log("worker start seq=%u pipe=%02x len=%lu timeout=%lu", b->seq, b->pipe,
             (unsigned long)b->length, (unsigned long)b->timeout);
    ok = rpc_on(s, &req, &resp, b->buffer, b->is_in ? 0 : b->length, b->is_in) == 0;
    if (!ok)
    {
        closesocket(s);
        worker_sock[index] = INVALID_SOCKET;
    }

    if (!ok)
    {
        shim_log("worker post seq=%u bytes=0 status=FAILED", b->seq);
        finish_bulk(b, STATUS_UNSUCCESSFUL, 0);
    }
    else if (resp.status == 0)
    {
        shim_log("worker post seq=%u bytes=%u status=OK", b->seq, resp.len);
        finish_bulk(b, STATUS_SUCCESS, resp.len);
    }
    else if (resp.status == -7 /* LIBUSB_ERROR_TIMEOUT */)
    {
        shim_log("worker post seq=%u bytes=%u status=TIMEOUT", b->seq, resp.len);
        finish_bulk(b, STATUS_IO_TIMEOUT, resp.len);
    }
    else if (resp.status == -10 /* LIBUSB_ERROR_INTERRUPTED */ || b->cancelled)
    {
        shim_log("worker post seq=%u bytes=0 status=CANCELLED", b->seq);
        finish_bulk(b, STATUS_CANCELLED, 0);
    }
    else if (b->is_in && resp.status == -1 /* LIBUSB_ERROR_IO, e.g. EPROTO */)
    {
        /* Real bus error: give libusb an NTSTATUS it understands.  Wine maps
         * STATUS_DEVICE_DATA_ERROR to ERROR_CRC, which the app turns into
         * LIBUSB_TRANSFER_ERROR (a plain, non-fatal error status). */
        shim_log("worker post seq=%u bytes=%u status=DATA_ERROR", b->seq, resp.len);
        finish_bulk(b, STATUS_DEVICE_DATA_ERROR, 0);
    }
    else
    {
        shim_log("worker post seq=%u bytes=%u status=ERR%d", b->seq, resp.len, resp.status);
        finish_bulk(b, STATUS_UNSUCCESSFUL, resp.len);
    }
}

static DWORD WINAPI bulk_worker(LPVOID param)
{
    int index = (int)(INT_PTR)param;

    for (;;)
    {
        struct bulk_ctx *b;

        WaitForSingleObject(q_sem, INFINITE);
        EnterCriticalSection(&q_lock);
        b = queue[q_head];
        queue[q_head] = NULL;
        q_head = (q_head + 1) % BULK_QUEUE_MAX;
        q_count--;
        LeaveCriticalSection(&q_lock);
        if (!b)
            continue;
        run_bulk_worker(b, index);
    }
    return 0;
}

static int queue_push(struct bulk_ctx *b)
{
    int ok = 0;

    EnterCriticalSection(&q_lock);
    if (q_count < BULK_QUEUE_MAX)
    {
        queue[q_tail] = b;
        q_tail = (q_tail + 1) % BULK_QUEUE_MAX;
        q_count++;
        ok = 1;
    }
    LeaveCriticalSection(&q_lock);
    if (ok)
        ReleaseSemaphore(q_sem, 1, NULL);
    return ok;
}

/* Synchronous bulk transfer on the shared control socket. */
static BOOL bulk_sync(struct ctx *c, UCHAR pipe, PUCHAR buffer, ULONG length,
                      ULONG *actual)
{
    struct shim_req req;
    struct shim_resp resp;
    int is_in = (pipe & 0x80) != 0;

    *actual = 0;
    if (length > SHIM_MAX_XFER)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    EnterCriticalSection(&lock);
    memset(&req, 0, sizeof(req));
    req.type = REQ_BULK;
    req.handle = c->handle;
    req.a = pipe;
    req.b = c->timeout_ms;
    req.len = length;
    if (rpc(&req, &resp, buffer, is_in ? 0 : length, is_in) || resp.status)
    {
        LeaveCriticalSection(&lock);
        SetLastError(resp.status == -7 ? ERROR_SEM_TIMEOUT : ERROR_GEN_FAILURE);
        return FALSE;
    }
    LeaveCriticalSection(&lock);
    *actual = resp.len;
    return TRUE;
}

static BOOL submit_async(struct ctx *c, UCHAR pipe, PUCHAR buffer, ULONG length,
                         LPOVERLAPPED overlapped, uint32_t seq)
{
    struct bulk_ctx *b;

    b = calloc(1, sizeof(*b));
    if (!b)
        return FALSE;
    b->dev = c;
    b->remote = c->handle;
    b->pipe = pipe;
    b->buffer = buffer;
    b->length = length;
    b->timeout = c->timeout_ms ? c->timeout_ms : 300000;
    b->is_in = (pipe & 0x80) != 0;
    b->seq = seq;
    b->overlapped = overlapped;

    overlapped->Internal = STATUS_PENDING;
    overlapped->InternalHigh = 0;
    if (overlapped->hEvent)
        ResetEvent(overlapped->hEvent);

    active_add(b);
    if (!queue_push(b))
    {
        shim_log("submit_async: queue full");
        active_remove(b);
        free(b);
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */

BOOL WINAPI WinUsb_Initialize(HANDLE file, PWINUSB_INTERFACE_HANDLE out)
{
    struct shim_req req;
    struct shim_resp resp;
    struct ctx *c;
    unsigned bus, addr;

    if (!out)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (parse_device_path(file, &bus, &addr))
    {
        shim_log("Initialize: not a shim device handle (err %lu)", GetLastError());
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    EnterCriticalSection(&lock);
    memset(&req, 0, sizeof(req));
    req.type = REQ_OPEN;
    req.a = bus;
    req.b = addr;
    req.c = VENDOR_ID;
    req.d = PRODUCT_ID;
    if (rpc(&req, &resp, NULL, 0, 0) || resp.status)
    {
        LeaveCriticalSection(&lock);
        shim_log("Initialize: open %u:%u failed status %d", bus, addr, resp.status);
        SetLastError(ERROR_DEVICE_NOT_CONNECTED);
        return FALSE;
    }
    c = ctx_alloc();
    if (!c)
    {
        LeaveCriticalSection(&lock);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->handle = resp.handle;
    c->speed = resp.speed;
    c->bus = resp.bus;
    c->addr = resp.addr;
    c->timeout_ms = 300000;
    c->file = file;

    memset(&req, 0, sizeof(req));
    req.type = REQ_DESCRIPTOR;
    req.handle = c->handle;
    req.a = 0x02;
    req.b = 0;
    req.len = sizeof(c->config);
    if (!rpc(&req, &resp, c->config, 0, 1) && !resp.status)
    {
        c->config_len = resp.len;
        parse_config(c);
    }
    LeaveCriticalSection(&lock);

    *out = (WINUSB_INTERFACE_HANDLE)c;
    shim_log("Initialize: opened %04x:%04x bus %u addr %u speed %u, %u pipes",
             VENDOR_ID, PRODUCT_ID, c->bus, c->addr, c->speed, c->num_pipes);
    return TRUE;
}

BOOL WINAPI WinUsb_Free(WINUSB_INTERFACE_HANDLE h)
{
    struct ctx *c = ctx_from(h);
    struct shim_req req;
    struct shim_resp resp;

    if (!c)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    shim_log("Free handle %p", (void *)h);
    EnterCriticalSection(&lock);
    memset(&req, 0, sizeof(req));
    req.type = REQ_CLOSE;
    req.handle = c->handle;
    rpc(&req, &resp, NULL, 0, 0);
    c->used = 0;
    LeaveCriticalSection(&lock);
    return TRUE;
}

BOOL WINAPI WinUsb_GetDescriptor(WINUSB_INTERFACE_HANDLE h, UCHAR type, UCHAR index,
                                 USHORT langid, PUCHAR buffer, ULONG length,
                                 PULONG transferred)
{
    struct ctx *c = ctx_from(h);
    struct shim_req req;
    struct shim_resp resp;

    if (!c || !buffer)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (length > SHIM_MAX_XFER)
        length = SHIM_MAX_XFER;
    EnterCriticalSection(&lock);
    memset(&req, 0, sizeof(req));
    req.type = REQ_DESCRIPTOR;
    req.handle = c->handle;
    req.a = type;
    req.b = index;
    req.c = langid;
    req.len = length;
    if (rpc(&req, &resp, buffer, 0, 1) || resp.status)
    {
        LeaveCriticalSection(&lock);
        shim_log("GetDescriptor type=%u index=%u -> FAIL status %d", type, index, resp.status);
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }
    LeaveCriticalSection(&lock);
    shim_log("GetDescriptor type=%u index=%u langid=%u -> %u bytes", type, index, langid,
             resp.len);
    if (transferred)
        *transferred = resp.len;
    return TRUE;
}

BOOL WINAPI WinUsb_QueryDeviceInformation(WINUSB_INTERFACE_HANDLE h, ULONG type,
                                          PULONG length, PVOID buffer)
{
    struct ctx *c = ctx_from(h);
    if (!c || !length || !buffer)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (type == DEVICE_SPEED)
    {
        if (*length < sizeof(ULONG))
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        *(ULONG *)buffer = c->speed;
        *length = sizeof(ULONG);
        return TRUE;
    }
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL WINAPI WinUsb_QueryInterfaceSettings(WINUSB_INTERFACE_HANDLE h, UCHAR alt,
                                          PUSB_INTERFACE_DESCRIPTOR desc)
{
    struct ctx *c = ctx_from(h);
    if (!c || !desc)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    shim_log("QueryInterfaceSettings alt=%u", alt);
    memset(desc, 0, sizeof(*desc));
    desc->bLength = sizeof(*desc);
    desc->bDescriptorType = 0x04;
    desc->bInterfaceNumber = 0;
    desc->bAlternateSetting = (UCHAR)alt;
    desc->bNumEndpoints = (UCHAR)c->num_pipes;
    desc->bInterfaceClass = 0xff;
    return TRUE;
}

BOOL WINAPI WinUsb_QueryPipe(WINUSB_INTERFACE_HANDLE h, UCHAR alt, UCHAR index,
                             PWINUSB_PIPE_INFORMATION info)
{
    struct ctx *c = ctx_from(h);
    if (!c || !info)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (index >= c->num_pipes)
    {
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    shim_log("QueryPipe alt=%u index=%u -> ep=%02x", alt, index, c->pipes[index].addr);
    memset(info, 0, sizeof(*info));
    info->PipeId = c->pipes[index].addr;
    info->PipeType = (USBD_PIPE_TYPE)c->pipes[index].type;
    info->MaximumPacketSize = c->pipes[index].maxpkt;
    info->Interval = c->pipes[index].interval;
    return TRUE;
}

BOOL WINAPI WinUsb_SetPipePolicy(WINUSB_INTERFACE_HANDLE h, UCHAR pipe, ULONG type,
                                 ULONG value_len, PVOID value)
{
    struct ctx *c = ctx_from(h);

    if (!c)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (type == PIPE_TRANSFER_TIMEOUT && value && value_len >= sizeof(ULONG))
    {
        c->timeout_ms = *(ULONG *)value;
        if (c->timeout_ms == 0 || c->timeout_ms > 300000)
            c->timeout_ms = 300000;
        shim_log("SetPipePolicy timeout pipe=%02x -> %lu", pipe,
                 (unsigned long)c->timeout_ms);
    }
    else
    {
        shim_log("SetPipePolicy pipe=%02x type=%lu len=%lu", pipe, (unsigned long)type,
                 (unsigned long)value_len);
    }
    return TRUE;
}

BOOL WINAPI WinUsb_GetPipePolicy(WINUSB_INTERFACE_HANDLE h, UCHAR pipe, ULONG type,
                                 PULONG value_len, PVOID value)
{
    struct ctx *c = ctx_from(h);
    (void)pipe;
    if (!c || !value_len || !value)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (type == PIPE_TRANSFER_TIMEOUT)
    {
        if (*value_len < sizeof(ULONG))
            return FALSE;
        *(ULONG *)value = c->timeout_ms;
        *value_len = sizeof(ULONG);
        return TRUE;
    }
    if (type == MAXIMUM_TRANSFER_SIZE)
    {
        if (*value_len < sizeof(ULONG))
            return FALSE;
        *(ULONG *)value = SHIM_MAX_XFER;
        *value_len = sizeof(ULONG);
        return TRUE;
    }
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

static BOOL read_write_pipe(WINUSB_INTERFACE_HANDLE h, UCHAR pipe, PUCHAR buffer,
                            ULONG length, PULONG transferred, LPOVERLAPPED overlapped)
{
    struct ctx *c = ctx_from(h);
    ULONG actual = 0;
    BOOL ok;

    shim_log("pipe call pipe=%02x len=%lu ov=%p ctx=%d", pipe, (unsigned long)length,
             (void *)overlapped, c != NULL);
    if (!c || (!buffer && length))
    {
        shim_log("pipe call: invalid ctx/buffer (ctx=%d buf=%p len=%lu)", c != NULL,
                 (void *)buffer, (unsigned long)length);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (length > SHIM_MAX_XFER)
    {
        shim_log("pipe call: length too large");
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    {
        uint32_t seq = c->seq_next[pipe]++;

        shim_log("submit seq=%u pipe=%02x len=%lu ov=%d", seq, pipe,
                 (unsigned long)length, overlapped != NULL);
        if (overlapped && submit_async(c, pipe, buffer, length, overlapped, seq))
        {
            DWORD e;
            shim_log("ReadPipe pipe=%02x len=%lu -> PENDING", pipe, (unsigned long)length);
            SetLastError(ERROR_IO_PENDING);
            e = GetLastError();
            shim_log("  GetLastError after SetLastError = %lu", (unsigned long)e);
            SetLastError(e); /* restore after logging */
            return FALSE;
        }
        shim_log("ReadPipe pipe=%02x len=%lu -> async submit FAILED, sync fallback", pipe,
                 (unsigned long)length);
    }

    ok = bulk_sync(c, pipe, buffer, length, &actual);
    if (transferred)
        *transferred = actual;
    if (overlapped)
    {
        overlapped->InternalHigh = actual; /* never rely on the transferred ptr */
        overlapped->Internal = ok ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
        MemoryBarrier();
        {
            HANDLE iocp;
            ULONG_PTR key;

            if (iocp_lookup(c->file, &iocp, &key))
                PostQueuedCompletionStatus(iocp, (DWORD)actual, key, overlapped);
            else if (overlapped->hEvent)
                SetEvent(overlapped->hEvent);
        }
    }
    return ok;
}

BOOL WINAPI WinUsb_ReadPipe(WINUSB_INTERFACE_HANDLE h, UCHAR pipe, PUCHAR buffer,
                            ULONG length, PULONG transferred, LPOVERLAPPED overlapped)
{
    return read_write_pipe(h, pipe, buffer, length, transferred, overlapped);
}

BOOL WINAPI WinUsb_WritePipe(WINUSB_INTERFACE_HANDLE h, UCHAR pipe, PUCHAR buffer,
                             ULONG length, PULONG transferred, LPOVERLAPPED overlapped)
{
    BOOL ok = read_write_pipe(h, pipe, buffer, length, transferred, overlapped);
    if (ok)
        shim_log("WritePipe pipe=%02x len=%lu -> %lu", pipe, (unsigned long)length,
                 transferred ? (unsigned long)*transferred : (unsigned long)length);
    return ok;
}

BOOL WINAPI WinUsb_ControlTransfer(WINUSB_INTERFACE_HANDLE h, WINUSB_SETUP_PACKET setup,
                                   PUCHAR buffer, ULONG length, PULONG transferred,
                                   LPOVERLAPPED overlapped)
{
    struct ctx *c = ctx_from(h);
    struct shim_req req;
    struct shim_resp resp;
    ULONG actual = 0;

    if (!c || (!buffer && length))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (length > SHIM_MAX_XFER)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    EnterCriticalSection(&lock);
    memset(&req, 0, sizeof(req));
    req.type = REQ_CONTROL;
    req.handle = c->handle;
    req.a = setup.RequestType;
    req.b = setup.Request;
    req.c = setup.Value;
    req.d = setup.Index;
    req.len = length ? length : setup.Length;
    if (req.len > SHIM_MAX_XFER)
        req.len = SHIM_MAX_XFER;
    if (rpc(&req, &resp, buffer, (setup.RequestType & 0x80) ? 0 : req.len,
            (setup.RequestType & 0x80) != 0) ||
        resp.status)
    {
        LeaveCriticalSection(&lock);
        shim_log("ControlTransfer rt=%02x req=%02x value=%04x index=%04x len=%lu -> FAIL %d",
                 setup.RequestType, setup.Request, setup.Value, setup.Index,
                 (unsigned long)length, resp.status);
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }
    LeaveCriticalSection(&lock);
    actual = resp.len;

    if (length && length <= 64)
    {
        char hex[256];
        ULONG k, n = length < 64 ? length : 64;
        for (k = 0; k < n; k++)
            sprintf(hex + k * 3, "%02x ", buffer[k]);
        shim_log("ControlTransfer rt=%02x req=%02x value=%04x index=%04x len=%lu -> %lu data=%s",
                 setup.RequestType, setup.Request, setup.Value, setup.Index,
                 (unsigned long)length, (unsigned long)actual, hex);
    }
    else
        shim_log("ControlTransfer rt=%02x req=%02x value=%04x index=%04x len=%lu -> %lu",
                 setup.RequestType, setup.Request, setup.Value, setup.Index,
                 (unsigned long)length, (unsigned long)actual);

    if (transferred)
        *transferred = actual;
    if (overlapped)
    {
        overlapped->InternalHigh = actual;
        overlapped->Internal = STATUS_SUCCESS;
        MemoryBarrier();
        if (overlapped->hEvent)
            SetEvent(overlapped->hEvent);
    }
    return TRUE;
}

static void abort_remote(struct ctx *c, UCHAR pipe)
{
    struct shim_req req;
    struct shim_resp resp;

    if (!c)
        return;
    EnterCriticalSection(&lock);
    memset(&req, 0, sizeof(req));
    req.type = REQ_ABORT_PIPE;
    req.handle = c->handle;
    req.a = pipe;
    rpc(&req, &resp, NULL, 0, 0);
    LeaveCriticalSection(&lock);
    shim_log("AbortPipe pipe=%02x (remote)", pipe);
}

/* Called from the CancelIoEx IAT hook (setupapi.dll).  Our "device file" has
 * no kernel I/O, so CancelIoEx alone would never complete the overlapped and
 * the app would hang on Stop.  Complete it here as STATUS_CANCELLED, then ask
 * the helper to abort the USB transfer so its worker thread unwinds. */
__declspec(dllexport) BOOL WINAPI shim_cancel_overlapped(LPOVERLAPPED ov)
{
    struct bulk_ctx *b, *hit = NULL;
    struct ctx *dev = NULL;
    UCHAR pipe = 0;
    HANDLE iocp = NULL;
    ULONG_PTR key = 0;
    int post = 0;

    if (!ov)
        return FALSE;
    EnterCriticalSection(&active_lock);
    for (b = active_list; b; b = b->next)
    {
        if (b->overlapped == ov)
        {
            hit = b;
            break;
        }
    }
    if (hit && !hit->completed)
    {
        hit->cancelled = 1;
        hit->completed = 1;
        dev = hit->dev;
        pipe = hit->pipe;
        ov->InternalHigh = 0;
        ov->Internal = STATUS_CANCELLED;
        MemoryBarrier();
        post = iocp_lookup(dev->file, &iocp, &key);
    }
    LeaveCriticalSection(&active_lock);

    if (!hit)
        return FALSE;

    shim_log("cancel overlay ov=%p pipe=%02x", ov, pipe);
    if (post)
        PostQueuedCompletionStatus(iocp, 0, key, ov);
    else if (ov->hEvent)
        SetEvent(ov->hEvent);

    abort_remote(dev, pipe);
    return TRUE;
}

BOOL WINAPI WinUsb_AbortPipe(WINUSB_INTERFACE_HANDLE h, UCHAR pipe)
{
    struct ctx *c = ctx_from(h);
    struct bulk_ctx *b;

    if (!c)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    shim_log("AbortPipe ENTER pipe=%02x", pipe);
    /* mark in-flight transfers; their workers complete them (no UAF) */
    EnterCriticalSection(&active_lock);
    for (b = active_list; b; b = b->next)
        if (b->dev == c && b->pipe == pipe)
            b->cancelled = 1;
    LeaveCriticalSection(&active_lock);

    abort_remote(c, pipe);
    shim_log("AbortPipe pipe=%02x", pipe);
    return TRUE;
}

BOOL WINAPI WinUsb_ResetPipe(WINUSB_INTERFACE_HANDLE h, UCHAR pipe)
{
    struct ctx *c = ctx_from(h);
    struct shim_req req;
    struct shim_resp resp;
    BOOL ok;

    if (!c)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    EnterCriticalSection(&lock);
    memset(&req, 0, sizeof(req));
    req.type = REQ_RESET_PIPE;
    req.handle = c->handle;
    req.a = pipe;
    ok = !rpc(&req, &resp, NULL, 0, 0) && !resp.status;
    LeaveCriticalSection(&lock);
    shim_log("ResetPipe pipe=%02x -> %d", pipe, ok);
    return ok;
}

BOOL WINAPI WinUsb_FlushPipe(WINUSB_INTERFACE_HANDLE h, UCHAR pipe)
{
    (void)pipe;
    return ctx_from(h) ? TRUE : FALSE;
}

BOOL WINAPI WinUsb_GetOverlappedResult(WINUSB_INTERFACE_HANDLE h,
                                       LPOVERLAPPED overlapped, LPDWORD transferred,
                                       BOOL wait)
{
    if (!ctx_from(h) || !overlapped)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (overlapped->Internal == STATUS_PENDING)
    {
        if (!wait)
        {
            SetLastError(ERROR_IO_INCOMPLETE);
            return FALSE;
        }
        if (overlapped->hEvent)
            WaitForSingleObject(overlapped->hEvent, INFINITE);
    }
    if (transferred)
        *transferred = (DWORD)overlapped->InternalHigh;

    switch (overlapped->Internal)
    {
        case STATUS_SUCCESS:
            return TRUE;
        case STATUS_IO_TIMEOUT:
            SetLastError(ERROR_SEM_TIMEOUT);
            return FALSE;
        case STATUS_CANCELLED:
            SetLastError(ERROR_OPERATION_ABORTED);
            return FALSE;
        case STATUS_DEVICE_DATA_ERROR:
            SetLastError(ERROR_CRC);
            return FALSE;
        default:
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
    }
}

BOOL WINAPI WinUsb_RegisterIsochBuffer(WINUSB_INTERFACE_HANDLE h, UCHAR pipe,
                                       PUCHAR buffer, ULONG length, PVOID *handle)
{
    (void)pipe; (void)buffer; (void)length; (void)handle;
    if (!ctx_from(h))
        return FALSE;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL WINAPI WinUsb_UnregisterIsochBuffer(PVOID handle)
{
    (void)handle;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL WINAPI WinUsb_SetCurrentAlternateSetting(WINUSB_INTERFACE_HANDLE h, UCHAR setting)
{
    shim_log("SetCurrentAlternateSetting %u", setting);
    return ctx_from(h) ? TRUE : FALSE;
}

BOOL WINAPI WinUsb_GetCurrentAlternateSetting(WINUSB_INTERFACE_HANDLE h, PUCHAR setting)
{
    if (!ctx_from(h) || !setting)
        return FALSE;
    *setting = 0;
    return TRUE;
}

BOOL WINAPI WinUsb_GetAssociatedInterface(WINUSB_INTERFACE_HANDLE h, UCHAR index,
                                          PWINUSB_INTERFACE_HANDLE out)
{
    (void)h; (void)index; (void)out;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        WSADATA wsa;
        int i;

        InitializeCriticalSection(&lock);
        InitializeCriticalSection(&q_lock);
        InitializeCriticalSection(&active_lock);
        InitializeCriticalSection(&log_lock);
        WSAStartup(MAKEWORD(2, 2), &wsa);
        CreateDirectoryA("C:\\ProgramData\\DSWaveShim", NULL);
        for (i = 0; i < BULK_WORKERS; i++)
            worker_sock[i] = INVALID_SOCKET;
        q_sem = CreateSemaphore(NULL, 0, BULK_QUEUE_MAX, NULL);
        for (i = 0; i < BULK_WORKERS; i++)
            workers[i] = CreateThread(NULL, 0, bulk_worker, (LPVOID)(INT_PTR)i, 0, NULL);
        {
            WCHAR exe[512];
            GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe));
            shim_log("winusb shim loaded in pid %lu exe=%ls", (unsigned long)GetCurrentProcessId(),
                     exe);
        }
        InterlockedExchange(&initialized, 1);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (InterlockedCompareExchange(&initialized, 0, 1))
        {
            int i;
            for (i = 0; i < BULK_WORKERS; i++)
                if (workers[i])
                    TerminateThread(workers[i], 0);
            if (sock != INVALID_SOCKET)
                closesocket(sock);
            WSACleanup();
        }
    }
    return TRUE;
}
