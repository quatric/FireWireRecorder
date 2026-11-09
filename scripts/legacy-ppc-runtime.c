/*
 * Runtime pieces the PowerPC build for Mac OS X 10.3 needs and the system
 * does not have (linked into every PowerPC binary by build-legacy-mac.sh).
 *
 * 1. The PowerPC crt1.o checks a GCC 3 hook, __darwin_gcc3_preregister_frame_info,
 *    and calls it if it is non-zero. In this toolchain it is a zero-filled
 *    common symbol, which the linker puts after all the zero-filled data: in
 *    ffmpeg that is more than 16 MB from the startup code, beyond the reach of
 *    its branch. A zero word in __DATA,__data sits close by and is skipped
 *    just the same.
 *
 * 2. 32-bit PowerPC has no 64-bit atomic instructions, so GCC calls
 *    __atomic_*_8 functions from libatomic, which Mac OS X never shipped.
 *    These do the same under one lock; ffmpeg uses them for a few counters.
 */
#include <pthread.h>
#include <stdint.h>

__attribute__((section("__DATA,__data"))) int __darwin_gcc3_preregister_frame_info = 0;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

#define BEGIN pthread_mutex_lock(&lock)
#define END pthread_mutex_unlock(&lock)

uint64_t atomic_load_8(const volatile void *p, int m) __asm__("___atomic_load_8");
uint64_t atomic_load_8(const volatile void *p, int m)
{
    (void)m;
    BEGIN;
    uint64_t v = *(const volatile uint64_t *)p;
    END;
    return v;
}

void atomic_store_8(volatile void *p, uint64_t v, int m) __asm__("___atomic_store_8");
void atomic_store_8(volatile void *p, uint64_t v, int m)
{
    (void)m;
    BEGIN;
    *(volatile uint64_t *)p = v;
    END;
}

uint64_t atomic_exchange_8(volatile void *p, uint64_t v, int m) __asm__("___atomic_exchange_8");
uint64_t atomic_exchange_8(volatile void *p, uint64_t v, int m)
{
    (void)m;
    BEGIN;
    uint64_t old = *(volatile uint64_t *)p;
    *(volatile uint64_t *)p = v;
    END;
    return old;
}

_Bool atomic_compare_exchange_8(volatile void *p, void *expected, uint64_t desired, int s, int f)
    __asm__("___atomic_compare_exchange_8");
_Bool atomic_compare_exchange_8(volatile void *p, void *expected, uint64_t desired, int s, int f)
{
    (void)s, (void)f;
    BEGIN;
    uint64_t cur = *(volatile uint64_t *)p;
    _Bool ok = cur == *(uint64_t *)expected;
    if (ok)
        *(volatile uint64_t *)p = desired;
    else
        *(uint64_t *)expected = cur;
    END;
    return ok;
}

#define FETCH_OP(name, op)                                                                        \
    uint64_t atomic_fetch_##name##_8(volatile void *p, uint64_t v, int m)                         \
        __asm__("___atomic_fetch_" #name "_8");                                                   \
    uint64_t atomic_fetch_##name##_8(volatile void *p, uint64_t v, int m)                         \
    {                                                                                             \
        (void)m;                                                                                  \
        BEGIN;                                                                                    \
        uint64_t old = *(volatile uint64_t *)p;                                                   \
        *(volatile uint64_t *)p = old op v;                                                       \
        END;                                                                                      \
        return old;                                                                               \
    }                                                                                             \
    uint64_t atomic_##name##_fetch_8(volatile void *p, uint64_t v, int m)                         \
        __asm__("___atomic_" #name "_fetch_8");                                                   \
    uint64_t atomic_##name##_fetch_8(volatile void *p, uint64_t v, int m)                         \
    {                                                                                             \
        (void)m;                                                                                  \
        BEGIN;                                                                                    \
        uint64_t n = *(volatile uint64_t *)p op v;                                                \
        *(volatile uint64_t *)p = n;                                                              \
        END;                                                                                      \
        return n;                                                                                 \
    }

FETCH_OP(add, +)
FETCH_OP(sub, -)
FETCH_OP(and, &)
FETCH_OP(or, |)
FETCH_OP(xor, ^)
