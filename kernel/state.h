// SPDX-License-Identifier: Apache-2.0
//
// Kernel object model.
//
// Everything the simulated kernel owns lives in fixed-capacity static pools.
// There is no malloc in the kernel: a run must not depend on allocation
// history, and the pools must be resettable without touching the heap.
//
// Handles are 64 bits: slot in the low half, generation in the high half. A
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
#define PERMUTE_MAX_REGISTRATIONS 1024 // secure events
#define PERMUTE_MAX_MUTEXES       128
#define PERMUTE_MAX_TIMERS        512  // absolute timer heap entries
#define PERMUTE_MAX_POSIX_TIMERS  128
#define PERMUTE_QUEUE_CAP         64   // receive queue depth
#define PERMUTE_NAME_MAX          32
#define PERMUTE_DEFAULT_PRIO_CEIL 63
#define PERMUTE_PRIO_MAX          63
#define PERMUTE_PRIO_BUCKETS      256  // ready-queue buckets (FR-3.11)
#define PERMUTE_RQ_NONE           0xFFFFu

// Clock period and RR quantum. FR-3.12 fixes the quantum at four clock
// periods; the period itself is a model constant (1 ms), because the target's
// is configurable.
#define PERMUTE_TICK_NS           1000000ull
#define PERMUTE_RR_QUANTUM_NS     (4ull * PERMUTE_TICK_NS)

// ---------------------------------------------------------------------------
// QNX ABI constants
//
// Values copied from <sys/neutrino.h> (channel and connect flags) and from the
// `_pulse` definition. They are part of the modelled ABI: a guest compares
// against them, so a wrong number breaks conformance silently.

// ChannelCreate flags.
#define PERMUTE_CHF_FIXED_PRIORITY  0x0001u
#define PERMUTE_CHF_UNBLOCK         0x0002u
#define PERMUTE_CHF_THREAD_DEATH    0x0004u
#define PERMUTE_CHF_DISCONNECT      0x0008u
#define PERMUTE_CHF_COID_DISCONNECT 0x0040u
#define PERMUTE_CHF_PRIVATE         0x1000u

// Kernel-generated pulse codes. Negative by construction; user codes are
// 0..127.
#define PERMUTE_PULSE_CODE_UNBLOCK     (-32) // value = rcvid
#define PERMUTE_PULSE_CODE_DISCONNECT  (-33) // value = server connection
#define PERMUTE_PULSE_CODE_THREADDEATH (-34) // value = thread id
#define PERMUTE_PULSE_CODE_COIDDEATH   (-35) // value = coid
#define PERMUTE_PULSE_CODE_MINAVAIL    0
#define PERMUTE_PULSE_CODE_MAXAVAIL    127

// struct _pulse on LP64: u16 type, u16 subtype, i8 code, u8 zero[3],
// union sigval value (8), i32 scoid, padded to 24.
#define PERMUTE_PULSE_WIRE_SIZE 24u

// sigevent notification types, in QNX's order (not POSIX's).
#define PERMUTE_SIGEV_NONE          0
#define PERMUTE_SIGEV_SIGNAL        1
#define PERMUTE_SIGEV_SIGNAL_CODE   2
#define PERMUTE_SIGEV_SIGNAL_THREAD 3
#define PERMUTE_SIGEV_PULSE         4
#define PERMUTE_SIGEV_UNBLOCK       5
#define PERMUTE_SIGEV_INTR          6
#define PERMUTE_SIGEV_THREAD        7
#define PERMUTE_SIGEV_MEMORY        8

// MsgRegisterEvent replaces the caller's event with an opaque handle. Our
// handle form is private to the kernel; the guest only ever passes it back.
#define PERMUTE_SIGEV_HANDLE 0x5A5Au

// ---------------------------------------------------------------------------
// TimerTimeout state mask
//
// The bits are (1 << STATE_*), with the values from <sys/states.h>. They are
// part of the modelled ABI: a guest ORs these together, so the numbers must
// match the target's.
#define PERMUTE_STATE_BIT_SEND        (1u << 4)
#define PERMUTE_STATE_BIT_RECEIVE     (1u << 5)
#define PERMUTE_STATE_BIT_REPLY       (1u << 6)
#define PERMUTE_STATE_BIT_SIGSUSPEND  (1u << 10)
#define PERMUTE_STATE_BIT_SIGWAITINFO (1u << 11)
#define PERMUTE_STATE_BIT_NANOSLEEP   (1u << 12)
#define PERMUTE_STATE_BIT_MUTEX       (1u << 13)
#define PERMUTE_STATE_BIT_CONDVAR     (1u << 14)
#define PERMUTE_STATE_BIT_JOIN        (1u << 15)
#define PERMUTE_STATE_BIT_INTR        (1u << 16)
#define PERMUTE_STATE_BIT_SEM         (1u << 17)

#define PERMUTE_TIMEOUT_MASK_ALL                                              \
    (PERMUTE_STATE_BIT_SEND | PERMUTE_STATE_BIT_RECEIVE |                     \
     PERMUTE_STATE_BIT_REPLY | PERMUTE_STATE_BIT_SIGSUSPEND |                 \
     PERMUTE_STATE_BIT_SIGWAITINFO | PERMUTE_STATE_BIT_NANOSLEEP |            \
     PERMUTE_STATE_BIT_MUTEX | PERMUTE_STATE_BIT_CONDVAR |                    \
     PERMUTE_STATE_BIT_JOIN | PERMUTE_STATE_BIT_INTR | PERMUTE_STATE_BIT_SEM)

// TimerTimeout flag: the time is absolute rather than relative.
#define PERMUTE_TIMER_ABSTIME 1u

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

// Result of an entry point that may block. Written into the caller's context
// when the operation completes without blocking, or into thread->pending when
// a blocked thread is resumed by someone else.
typedef enum {
    PERMUTE_CONTINUE = 0,
    PERMUTE_BLOCKED = 1
} permute_action_t;

typedef struct {
    permute_action_t action;
    long ret;          // status for send/reply; rcvid for receive
    int err;           // 0 or errno
    int32_t rcvid;     // receive: 0 = pulse, >0 = message
    uint32_t bytes;    // bytes copied into the caller's buffer
    struct {
        int32_t code;
        uint64_t value;
        int32_t priority;
        int32_t scoid;  // server connection the pulse arrived on
    } pulse;
} permute_result_t;

// Normalised QNX `struct sigevent`, 32 bytes on LP64 and layout-compatible
// with the platform header: i32 notify, 8-byte union (signo/coid/...),
// union sigval value, 8-byte union (code+priority, or attributes pointer).
//
// The reserved words are padding in the C struct, so two events that are
// semantically identical can differ byte-wise. Compare fields, not bytes -
// the PRD says "byte compare", and this is that comparison with the
// uninitialised padding removed.
typedef struct {
    int32_t  notify;
    uint32_t reserved0;
    int32_t  coid;      // sigev_coid; also carries our registration handle
    uint32_t reserved1;
    uint64_t value;     // sigev_value
    int16_t  code;      // sigev_code
    int16_t  priority;  // sigev_priority
    uint32_t reserved2;
} permute_sigevent_t;

_Static_assert(sizeof(permute_sigevent_t) == 32,
               "sigevent must match the 32-byte QNX layout");

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
    permute_handle_t process;   // owning process handle
    permute_handle_t self;      // this thread's own handle

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
    uint64_t quantum_left_ns; // RR only; resets on dispatch

    permute_handle_t wobj;   // wait object: channel, mutex, cond, ...
    int32_t wrcvid;          // message being replied to, while in REPLY
    permute_handle_t wmsg;   // message this thread sent, while in SEND/REPLY

    // Receive context. Captured when the thread blocks in MsgReceive and used
    // when a message or pulse is delivered, because the delivery happens while
    // the thread is not running.
    void *recv_buf;
    uint32_t recv_max;
    uint32_t recv_len;
    int32_t recv_rcvid;      // 0 = pulse, > 0 = message
    uint8_t recv_kind;       // what this thread is willing to receive
    uint8_t recv_short;      // MsgReceivePulse with a sub-_pulse buffer

    // Reply context. Captured at send time and used when the server replies.
    // The reply buffer pointer must survive until then; the message payload
    // itself is copied at send (FR-3.7).
    void *reply_buf;
    uint32_t reply_max;
    uint32_t reply_len;
    long reply_status;
    int32_t reply_err;       // 0 = MsgReply, else errno from MsgError

    uint64_t timeout_deadline_ns; // 0 = disarmed
    uint32_t timeout_mask;
    uint64_t timeout_seq;         // generation of the live heap entry
    uint8_t timeout_armed;

    permute_result_t pending;     // valid when this thread was unblocked

    void *host_ctx;
} permute_thread_t;

typedef struct {
    permute_handle_t handle;
    int32_t priority;
    uint64_t sequence;
    uint8_t kind;   // 0 message, 1 pulse, 2 thread
} permute_queue_entry_t;

// Queue entry kinds. Messages and pulses are items; threads are waiters.
// The same byte marks both, so a single mask argument can mean "any waiter
// that accepts a pulse" or "only waiters that accept messages".
#define PERMUTE_Q_MESSAGE    0x01u // queued message
#define PERMUTE_Q_PULSE      0x02u // queued pulse
#define PERMUTE_Q_THREAD     0x04u // waiter: MsgReceive, takes messages and pulses
#define PERMUTE_Q_PULSE_RECV 0x08u // waiter: MsgReceivePulse, takes pulses only
#define PERMUTE_Q_ANY        0xFFu

typedef struct {
    permute_queue_entry_t entries[PERMUTE_QUEUE_CAP];
    uint32_t length;
} permute_queue_t;

// Ready queue: one FIFO bucket per priority level plus an occupancy bitmap.
//
// Pop is O(1): the highest non-zero bitmap word is found in at most four
// iterations, the highest set bit in one `clz`, and the winner is that
// bucket's head. Nothing scans the thread table.
//
// Links are indexed by thread slot and owned by the queue, so an entry is
// always a valid index and removal from the middle is O(1). Buckets store
// the slot's priority as well, which is what makes removal possible without
// a scan.
typedef struct {
    uint16_t head[PERMUTE_PRIO_BUCKETS];
    uint16_t tail[PERMUTE_PRIO_BUCKETS];
    uint16_t count[PERMUTE_PRIO_BUCKETS];
    uint16_t prio[PERMUTE_MAX_THREADS];
    uint16_t next[PERMUTE_MAX_THREADS];
    uint16_t prev[PERMUTE_MAX_THREADS];
    uint8_t  on[PERMUTE_MAX_THREADS];
    uint64_t bitmap[PERMUTE_PRIO_BUCKETS / 64];
    uint32_t total;
} permute_readyq_t;

typedef struct {
    uint32_t chid;        // owner-local index
    uint32_t owner_pid;
    uint32_t flags;
    uint32_t conn_refs;
    permute_queue_t recvq;        // pending messages and pulses, priority-ordered
    permute_queue_t recv_waiters; // threads blocked in MsgReceive, priority-ordered
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
    uint32_t receiver_tid;
    permute_handle_t channel;  // channel the message is queued on, if any
    uint32_t payload_len;
    uint8_t *payload;   // owned by the kernel blob arena
    uint32_t reply_len;
    uint8_t *reply;     // allocated on reply, may be NULL
} permute_message_t;

typedef struct {
    int32_t code;
    uint64_t value;     // pointer-sized value, never dereferenced
    int32_t priority;
    int32_t scoid;      // connection the pulse was sent on, 0 if local
    uint32_t src_pid;
    uint32_t src_tid;
    uint64_t sequence;
} permute_pulse_t;

// A secure event, registered by the client and triggerable by one server.
// Keyed by (owner thread, connection) as the PRD requires, but looked up by
// handle: QNX replaces the caller's sigevent with an opaque handle, so the
// handle is what travels in messages.
typedef struct {
    uint32_t handle;
    uint32_t owner_pid;
    uint32_t owner_tid;
    uint32_t allowed_pid;   // the only server permitted to deliver it
    int32_t allowed_coid;   // connection it was registered against
    permute_sigevent_t event;
} permute_registration_t;

// Mutex protocol. NORMAL is the default: no inheritance. INHERIT boosts the
// owner to the highest blocked waiter and reverts on unlock. PROTECT raises
// the locker to the mutex ceiling for as long as it holds it.
typedef enum {
    PERMUTE_MUTEX_NORMAL = 0,
    PERMUTE_MUTEX_INHERIT,
    PERMUTE_MUTEX_PROTECT
} permute_mutex_proto_t;

typedef struct {
    permute_handle_t owner;   // thread handle, or NONE
    uint32_t owner_pid;
    uint32_t lock_count;      // recursion depth
    permute_mutex_proto_t proto;
    int32_t ceiling;          // meaningful for PROTECT
    uint8_t recursive;
    permute_queue_t waiters;  // blocked threads, priority-ordered
} permute_mutex_t;

// Timer heap entry. Thread timeouts and POSIX timers share one absolute-time
// heap so that the clock has exactly one notion of "next event".
typedef enum {
    PERMUTE_TIMER_THREAD = 0, // TimerTimeout on a thread
    PERMUTE_TIMER_POSIX = 1   // POSIX timer_create/settime
} permute_timer_kind_t;

typedef struct {
    uint64_t deadline_ns;
    uint64_t seq;             // invalidates superseded entries lazily
    permute_handle_t target;  // thread handle or POSIX timer handle
    uint8_t kind;
} permute_timer_entry_t;

typedef struct {
    permute_timer_entry_t heap[PERMUTE_MAX_TIMERS];
    uint32_t count;
} permute_eventq_t;

typedef struct {
    permute_sigevent_t event;
    uint64_t interval_ns;     // 0 = one-shot
    uint64_t next_ns;
    uint64_t seq;
    permute_handle_t owner_process;
    uint32_t owner_pid;
    uint8_t armed;
} permute_posix_timer_t;

// ---------------------------------------------------------------------------
// Kernel

typedef struct {
    permute_table_t processes;
    permute_table_t threads;
    permute_table_t channels;
    permute_table_t connections;
    permute_table_t messages;
    permute_table_t pulses;
    permute_table_t registrations;
    permute_table_t mutexes;
    permute_table_t posix_timers;

    permute_process_t process_pool[PERMUTE_MAX_PROCESSES];
    permute_thread_t thread_pool[PERMUTE_MAX_THREADS];
    permute_channel_t channel_pool[PERMUTE_MAX_CHANNELS];
    permute_connection_t connection_pool[PERMUTE_MAX_CONNECTIONS];
    permute_message_t message_pool[PERMUTE_MAX_MESSAGES];
    permute_pulse_t pulse_pool[PERMUTE_MAX_PULSES];
    permute_registration_t registration_pool[PERMUTE_MAX_REGISTRATIONS];
    permute_mutex_t mutex_pool[PERMUTE_MAX_MUTEXES];
    permute_posix_timer_t posix_timer_pool[PERMUTE_MAX_POSIX_TIMERS];

    permute_readyq_t readyq;
    permute_handle_t running;   // currently dispatched thread, or NONE

    permute_eventq_t eventq;
    uint64_t now_ns;            // virtual clock; advances only when idle

    uint32_t next_pid;
    uint32_t next_tid;
    uint32_t next_registration;
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
