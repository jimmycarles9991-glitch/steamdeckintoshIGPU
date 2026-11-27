/* SPDX-License-Identifier: MIT */
#include "vg_bootargs.h"
#include <stddef.h>

static int is_sep(char c) { return c == ' ' || c == '\t' || c == '\n' || c == 0; }

int vg_arg_has(const char *args, const char *token) {
    if (!args || !token || !*token) return 0;
    size_t tlen = __builtin_strlen(token);
    const char *p = args;
    while (*p) {
        while (*p && is_sep(*p)) ++p;
        const char *s = p;
        while (*p && !is_sep(*p)) ++p;
        size_t n = (size_t)(p - s);
        if (n == tlen) {
            size_t i = 0;
            while (i < n && s[i] == token[i]) ++i;
            if (i == n) return 1;
        }
    }
    return 0;
}

int vg_arg_u32(const char *args, const char *key, uint32_t *out) {
    if (!args || !key || !out || !*key) return -1;
    size_t klen = __builtin_strlen(key);
    for (const char *p = args; *p;) {
        while (*p && is_sep(*p)) ++p;
        const char *s = p;
        while (*p && !is_sep(*p)) ++p;
        size_t n = (size_t)(p - s);
        if (n > klen + 1 && s[klen] == '=') {
            size_t i = 0;
            while (i < klen && s[i] == key[i]) ++i;
            if (i == klen) {
                const char *v = s + klen + 1;
                int base = 10;
                if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) { base = 16; v += 2; }
                uint32_t acc = 0;
                int any = 0;
                for (; *v && !is_sep(*v); ++v) {
                    int d;
                    if (*v >= '0' && *v <= '9') d = *v - '0';
                    else if (base == 16 && *v >= 'a' && *v <= 'f') d = *v - 'a' + 10;
                    else if (base == 16 && *v >= 'A' && *v <= 'F') d = *v - 'A' + 10;
                    else return -1;
                    if (acc > (0xffffffffu - (uint32_t)d) / (uint32_t)base) return -1;
                    acc = acc * (uint32_t)base + (uint32_t)d;
                    any = 1;
                }
                if (any) { *out = acc; return 0; }
            }
        }
    }
    return -1;
}
