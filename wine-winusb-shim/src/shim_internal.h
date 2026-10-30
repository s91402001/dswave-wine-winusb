#ifndef DSWAVE_SHIM_INTERNAL_H
#define DSWAVE_SHIM_INTERNAL_H

#include <windows.h>
#include "proto.h"
#include "shim_ids.h"

/* implemented in setupapi_shim.c, used by ioctl_hook.c */
int shim_get_dev_info(struct dev_info *out);
const WCHAR *shim_hub_path(void);

/* implemented in ioctl_hook.c, called from setupapi_shim.c */
void ioctl_hook_install(void);

/* I/O completion port association captured from CreateIoCompletionPort() */
int shim_lookup_iocp(HANDLE file, HANDLE *iocp, ULONG_PTR *key);
void shim_forget_iocp(HANDLE file);

#endif
