#ifndef CTSC_COMMON_H
#define CTSC_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#if defined(_MSC_VER)
#  define CTSC_NORETURN __declspec(noreturn)
#else
#  define CTSC_NORETURN __attribute__((noreturn))
#endif

CTSC_NORETURN void ctsc_panic(const char* file, int line, const char* fmt, ...);

#define CTSC_PANIC(...) ctsc_panic(__FILE__, __LINE__, __VA_ARGS__)
#define CTSC_ASSERT(cond)                                                      \
    do {                                                                       \
        if (!(cond)) { CTSC_PANIC("assertion failed: %s", #cond); }            \
    } while (0)

#define CTSC_UNUSED(x) ((void)(x))

#endif
