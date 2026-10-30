#ifndef DSWAVE_SHIM_PROTO_H
#define DSWAVE_SHIM_PROTO_H

#include <stdint.h>

#define SHIM_PORT 47123
#define SHIM_DEV_DESC_LEN 18
#define SHIM_CONFIG_MAX 4096

enum
{
    REQ_LIST = 1,
    REQ_OPEN,
    REQ_CLOSE,
    REQ_CONTROL,
    REQ_BULK,
    REQ_DESCRIPTOR,
    REQ_ABORT_PIPE,   /* a = endpoint */
    REQ_RESET_PIPE,   /* a = endpoint */
};

#pragma pack(push, 1)
struct shim_req
{
    uint32_t type;
    uint32_t handle;
    uint32_t a, b, c, d;
    uint32_t len;
};

struct shim_resp
{
    int32_t status;      /* 0 = ok, else libusb error code (negative) or errno */
    uint32_t len;
    uint32_t handle;
    uint32_t bus, addr, vid, pid, speed, interfaces;
};

/* Returned by REQ_LIST.  Descriptors are raw bytes read from the device at
 * enumeration time, so the Windows side can answer hub IOCTLs without an
 * open handle. */
struct dev_info
{
    uint32_t bus, addr, vid, pid, speed, interfaces;
    uint32_t config_len;
    uint8_t device_desc[SHIM_DEV_DESC_LEN];
    uint8_t config_desc[SHIM_CONFIG_MAX];
};
#pragma pack(pop)

#define SHIM_MAX_XFER (64u * 1024 * 1024)

#endif
