// SPDX-License-Identifier: Apache-2.0
//
// Clean-room QNX-compatible declarations needed by guest sources.
//
// Only the constants, structures, and initialiser macros that the modelled
// surface uses. Values are from public documentation, never copied from a
// vendor header (ADR-011). On a real target the vendor header provides the
// same names, so a scenario source compiles unchanged for both.
//
// glibc forward-declares struct sigevent but does not define it in the headers
// this file pulls in, so defining it here is safe. Do not include <signal.h>
// in a guest that uses this header: glibc defines a different sigevent there.

#ifndef PERMUTE_QNX_H
#define PERMUTE_QNX_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

union sigval {
    int sival_int;
    void *sival_ptr;
    long sival_long;
    unsigned long sival_ulong;
};

// QNX sigevent: 32 bytes on LP64. The anonymous unions give the field names
// the vendor header exposes through macros.
struct sigevent {
    int sigev_notify;
    union {
        int sigev_signo;
        int sigev_coid;
        int sigev_id;
        void (*sigev_notify_function)(union sigval);
        volatile unsigned *sigev_addr;
    };
    union sigval sigev_value;
    union {
        struct {
            short sigev_code;
            short sigev_priority;
        };
        void *sigev_notify_attributes;
        int sigev_memop;
    };
};

_Static_assert(sizeof(struct sigevent) == 32,
               "sigevent must match the QNX 32-byte layout");

// struct _pulse: 24 bytes on LP64.
struct _pulse {
    uint16_t type;
    uint16_t subtype;
    int8_t code;
    uint8_t zero[3];
    union sigval value;
    int32_t scoid;
};

_Static_assert(sizeof(struct _pulse) == 24,
               "_pulse must match the QNX 24-byte layout");

// Notification types, in QNX's order (not POSIX's).
#define SIGEV_NONE 0
#define SIGEV_SIGNAL 1
#define SIGEV_SIGNAL_CODE 2
#define SIGEV_SIGNAL_THREAD 3
#define SIGEV_PULSE 4
#define SIGEV_UNBLOCK 5
#define SIGEV_INTR 6
#define SIGEV_THREAD 7
#define SIGEV_MEMORY 8

#define SIGEV_PULSE_INIT(__e, __coid, __prio, __code, __value)                 \
    do {                                                                       \
        (__e)->sigev_notify = SIGEV_PULSE;                                     \
        (__e)->sigev_coid = (__coid);                                          \
        (__e)->sigev_priority = (__prio);                                      \
        (__e)->sigev_code = (__code);                                          \
        (__e)->sigev_value.sival_ptr = (void *)(uintptr_t)(__value);           \
    } while (0)

#define SIGEV_UNBLOCK_INIT(__e)                                                \
    do {                                                                       \
        (__e)->sigev_notify = SIGEV_UNBLOCK;                                   \
    } while (0)

// TimerTimeout state mask. Values are (1 << STATE_*).
#define _NTO_TIMEOUT_SEND        (1u << 4)
#define _NTO_TIMEOUT_RECEIVE     (1u << 5)
#define _NTO_TIMEOUT_REPLY       (1u << 6)
#define _NTO_TIMEOUT_SIGSUSPEND  (1u << 10)
#define _NTO_TIMEOUT_SIGWAITINFO (1u << 11)
#define _NTO_TIMEOUT_NANOSLEEP   (1u << 12)
#define _NTO_TIMEOUT_MUTEX       (1u << 13)
#define _NTO_TIMEOUT_CONDVAR     (1u << 14)
#define _NTO_TIMEOUT_JOIN        (1u << 15)
#define _NTO_TIMEOUT_INTR        (1u << 16)
#define _NTO_TIMEOUT_SEM         (1u << 17)

#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 0x01
#endif

// ChannelCreate flags.
#define _NTO_CHF_FIXED_PRIORITY  0x0001u
#define _NTO_CHF_UNBLOCK         0x0002u
#define _NTO_CHF_THREAD_DEATH    0x0004u
#define _NTO_CHF_DISCONNECT      0x0008u
#define _NTO_CHF_COID_DISCONNECT 0x0040u
#define _NTO_CHF_PRIVATE         0x1000u

// Kernel-generated pulse codes.
#define _PULSE_TYPE 0
#define _PULSE_SUBTYPE 0
#define _PULSE_CODE_UNBLOCK     (-32)
#define _PULSE_CODE_DISCONNECT  (-33)
#define _PULSE_CODE_THREADDEATH (-34)
#define _PULSE_CODE_COIDDEATH   (-35)
#define _PULSE_CODE_MINAVAIL    0
#define _PULSE_CODE_MAXAVAIL    127

#ifdef __cplusplus
}
#endif

#endif // PERMUTE_QNX_H
