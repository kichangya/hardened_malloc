#ifndef UTIL_H
#define UTIL_H

#include <stdnoreturn.h>

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define COLD __attribute__((cold))
#define UNUSED __attribute__((unused))
#define EXPORT __attribute__((visibility("default")))

#define STRINGIFY(s) #s
#define ALIAS(f) __attribute__((alias(STRINGIFY(f))))

static inline int ffzl(long x) {
    return __builtin_ffsl(~x);
}

COLD noreturn void fatal_error(const char *s);

#endif
