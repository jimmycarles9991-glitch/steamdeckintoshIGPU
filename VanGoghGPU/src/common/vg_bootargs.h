/* SPDX-License-Identifier: MIT */
#ifndef VG_BOOTARGS_H
#define VG_BOOTARGS_H

#include <stdint.h>

/* Bare-word token (e.g. "-vgfboff" or "vgfbown"). */
int vg_arg_has(const char *args, const char *token);

/* "key=value" decimal or 0x hex. Returns 0 and fills *out on success. */
int vg_arg_u32(const char *args, const char *key, uint32_t *out);

#endif
