// Minimal assertion helper shared by the host tests.
#ifndef VANGOGH_HOST_CHECK_H
#define VANGOGH_HOST_CHECK_H
#include <stdio.h>

static int g_failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_failures++; } } while (0)
#define CHECK_EQ(a, b) do { unsigned long long _a = (unsigned long long)(a), _b = (unsigned long long)(b); \
    if (_a != _b) { printf("FAIL %s:%d: %s == %s (0x%llx vs 0x%llx)\n", __FILE__, __LINE__, #a, #b, _a, _b); g_failures++; } } while (0)
#define FINISH(name) do { printf("%s: %s\n", name, g_failures ? "FAILED" : "ok"); return g_failures ? 1 : 0; } while (0)
#endif
