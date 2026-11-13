// SPDX-License-Identifier: Apache-2.0
//
// Kernel object model.
//
// Everything the simulated kernel owns lives in fixed-capacity static pools.
// There is no malloc in the kernel: a run must not depend on allocation
// history, and the pools must be resettable without touching the heap.
//
// Handles are 32 bits: generation in the high half, slot in the low half. A
// freed slot keeps its generation, so a stale handle is detected instead of
// silently addressing a different object.
//
// Receive IDs are a separate encoding because they must be positive int32:
// slot+1 in the low 16 bits, generation in the high 15. The sign bit stays
// clear, and the generation wraps at 32768 - documented, not hidden.

#ifndef PERMUTE_STATE_H
#define PERMUTE_STATE_H

#include <stdint.h>

#include "kernel/blob.h"
#include "kernel/decisions.h"

// Pools. Sized for a service graph, not for a desktop: exceeding one is a
// hard -EMFILE/-ENOMEM, never a silent drop.
#define PERMUTE_MAX_SLOTS         1024
#define PERMUTE_MAX_PROCESSES     64
#define PERMUTE_MAX_THREADS       256
#define PERMUTE_MAX_CHANNELS      256
#define PERMUTE_MAX_CONNECTIONS   1024
#define PERMUTE_MAX_MESSAGES      1024
#define PERMUTE_MAX_PULSES        1024
#define PERMUTE_MAX_CH            64   // channels per process (chid space)
#define PERMUTE_MAX_CO            1024 // connections per process (coid/fd space)
#define PERMUTE_QUEUE_CAP         64   // receive queue depth
#define PERMUTE_NAME_MAX          32
#define PERMUTE_DEFAULT_PRIO_CEIL 63
#define PERMUTE_PRIO_MAX          63

// ---------------------------------------------------------------------------
// Handles
//
// 64-bit: slot in the low half, 32-bit generation in the high half. A freed
// slot increments its generation, so a stale handle is rejected rather than
// silently addressing a different object. 32 bits of generation means the
// counter cannot wrap within any realistic run, which is what "generation
// never repeats" requires.

typedef uint64_t permute_handle_t;

#define PERMUTE_HANDLE_NONE UINT64_MAX
#define PERMUTE_HANDLE_SLOT(h) ((uint32_t)((h) & 0xFFFFFFFFu))
#define PERMUTE_HANDLE_GEN(h)  ((uint32_t)((h) >> 32))
#define PERMUTE_HANDLE_MAKE(slot, gen)                                        \
    ((((uint64_t)(uint32_t)(gen)) << 32) | (uint32_t)(slot))

// Receive IDs: positive int32, 0 means "this was a pulse".
#define PERMUTE_RCVID_GEN_MASK 0x7FFFu

static inline int32_t permute_rcvid_make(uint32_t slot, uint32_t gen)
{
    return (int32_t)(((gen & PERMUTE_RCVID_GEN_MASK) << 16) |
                     ((slot + 1u) & 0xFFFFu));
}

static inline int permute_rcvid_is_valid(int32_t rcvid)
{
    return rcvid > 0;
}

static inline uint32_t permute_rcvid_slot(int32_t rcvid)
{
    return (((uint32_t)rcvid) & 0xFFFFu) - 1u;
}

static inline uint32_t permute_rcvid_gen(int32_t rcvid)
{
    return (((uint32_t)rcvid) >> 16) & PERMUTE_RCVID_GEN_MASK;
}

// ---------------------------------------------------------------------------
// Generic handle table

typedef struct {
    uint32_t capacity;
    uint32_t free_head;
    uint32_t live;
    uint32_t next_free[PERMUTE_MAX_SLOTS];
    uint32_t generation[PERMUTE_MAX_SLOTS];
    uint8_t  in_use[PERMUTE_MAX_SLOTS];
} permute_table_t;
// ---------------------------------------------------------------------------
// Threads and scheduling

typedef enum {
    PERMUTE_THREAD_DEAD = 0,
    PERMUTE_THREAD_RUNNING,
    PERMUTE_THREAD_READY,
    PERMUTE_THREAD_SEND,
    PERMUTE_THREAD_REPLY,
    PERMUTE_THREAD_RECEIVE,
    PERMUTE_THREAD_NANOSLEEP,
    PERMUTE_THREAD_MUTEX,
    PERMUTE_THREAD_COND,
    PERMUTE_THREAD_SEM,
    PERMUTE_THREAD_JOIN,
    PERMUTE_THREAD_INTR,
    PERMUTE_THREAD_STOPPED,
    PERMUTE_THREAD_ZOMBIE
} permute_thread_state_t;

typedef enum {
    PERMUTE_SCHED_FIFO = 0,
    PERMUTE_SCHED_RR,
    PERMUTE_SCHED_OTHER
} permute_policy_t;

// Ability bits. Only the one that matters for priority clamping is defined.
#define PERMUTE_AID_PRIORITY 0x00000001u

// ---------------------------------------------------------------------------
// Objects

typedef struct {
    uint32_t pid;
    char name[PERMUTE_NAME_MAX];
    void *dl_handle;

    permute_handle_t channels[PERMUTE_MAX_CH];      // chid -> channel
    permute_handle_t connections[PERMUTE_MAX_CO];   // coid (== fd) -> connection

    permute_stream_t decisions;                     // counter-based, per process
    int32_t prio_ceiling;
    uint32_t abilities;
} permute_process_t;

typedef struct {
    uint32_t tid;
    uint32_t pid;

    int32_t base_prio;
    int32_t sticky_prio;   // message-driven; does NOT revert on reply
    int32_t mutex_boost;   // classic PI; reverts on unlock
    int32_t prot_ceil;     // max ceiling of held PROTECT mutexes
    int32_t eff_prio;

    permute_policy_t policy;
    permute_thread_state_t state;
    uint32_t sched_flags;

    uint64_t cpu_ns;
    uint64_t quantum_ns;

    permute_handle_t wobj;   // wait object: channel, mutex, cond, ...
    int32_t rcvid;           // for REPLY state
    uint64_t timeout_deadline_ns; // 0 = disarmed
    uint32_t timeout_mask;

    void *host_ctx;
} permute_thread_t;

typedef struct {
    permute_handle_t entries[PERMUTE_QUEUE_CAP];
    uint32_t head;
    uint32_t tail;
    uint32_t length;
} permute_queue_t;

typedef struct {
    uint32_t chid;        // owner-local index
    uint32_t owner_pid;
    uint32_t flags;
    uint32_t receive_waiters;
    uint32_t conn_refs;
    permute_queue_t recvq;
} permute_channel_t;

typedef struct {
    int32_t coid;          // client-local index, equal to the fd value
    uint32_t client_pid;
    uint32_t client_tid;
    uint32_t server_pid;
    permute_handle_t channel;
    uint32_t flags;
    uint32_t refs;
} permute_connection_t;

typedef struct {
    uint32_t src_pid;
    uint32_t src_tid;
    uint32_t dst_pid;
    uint32_t dst_tid;
    int32_t send_prio;
    uint64_t sequence;
    int32_t rcvid;
    uint8_t replied;
    uint8_t in_receive_queue;
    uint32_t payload_len;
    uint8_t *payload;   // owned by the kernel blob arena
    uint32_t reply_len;
    uint8_t *reply;     // allocated on reply, may be NULL
} permute_message_t;

typedef struct {
    int32_t code;
    uint64_t value;     // pointer-sized value, never dereferenced
    int32_t priority;
    uint32_t src_pid;
    uint32_t src_tid;
    uint64_t sequence;
} permute_pulse_t;

// ---------------------------------------------------------------------------
// Kernel

typedef struct {
    permute_table_t processes;
    permute_table_t threads;
    permute_table_t channels;
    permute_table_t connections;
    permute_table_t messages;
    permute_table_t pulses;

    permute_process_t process_pool[PERMUTE_MAX_PROCESSES];
    permute_thread_t thread_pool[PERMUTE_MAX_THREADS];
    permute_channel_t channel_pool[PERMUTE_MAX_CHANNELS];
    permute_connection_t connection_pool[PERMUTE_MAX_CONNECTIONS];
    permute_message_t message_pool[PERMUTE_MAX_MESSAGES];
    permute_pulse_t pulse_pool[PERMUTE_MAX_PULSES];

    uint32_t next_pid;
    uint32_t next_tid;
    uint64_t sequence;
    uint64_t seed;

    permute_blob_arena_t blobs;
} permute_kernel_t;

// Effective priority: max(base, sticky, mutex boost, protect ceiling).
static inline int32_t permute_eff_prio(const permute_thread_t *t)
{
    int32_t p = t->base_prio;
    if (t->sticky_prio > p)
        p = t->sticky_prio;
    if (t->mutex_boost > p)
        p = t->mutex_boost;
    if (t->prot_ceil > p)
        p = t->prot_ceil;
    return p;
}

#endif // PERMUTE_STATE_H
