<!-- SPDX-License-Identifier: Apache-2.0 -->

# Kernel contract

**Owner:** [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14)
**Status:** current for v0.1. Normative for the bridge and the kernel model; derived from `abi/permute_abi.h`, `runtime/bridge.c` and `kernel/*.h`.
**Companion documents:** [`ABI.md`](ABI.md) (guest surface), [`ARCHITECTURE.md`](ARCHITECTURE.md) §3–§7 (design), [`DETERMINISM.md`](DETERMINISM.md) (invariants).

This is the contract between the guest-side shim, the host bridge, the kernel
model, and the executor. It is the layer where a mistake becomes either
nondeterminism or a silent semantic approximation, so every rule here is
enforced by code or by a test named in the row.

---

## 1. The four participants

| Participant | Runs on | Owns | Must never do |
|---|---|---|---|
| shim (`abi/shim_*.c`) | guest thread | nothing | keep state, retain a payload pointer, call libc |
| bridge (`runtime/bridge.c`) | guest thread | the call path, trace emission, fault application | choose the next thread, read a host clock |
| kernel (`kernel/*.c`) | caller's stack | object state, ready queue, timers, decisions | block, allocate from libc, call the host |
| executor (`runtime/executor.c`) | host threads | the baton, the host thread table, the sweep | let two guest threads run at once |

The single rule that makes the model sound: **the kernel never selects the next
thread.** It updates state and returns `Continue` or `Blocked`; the executor
picks from the ready queue. A kernel that picked would need to know about host
threads, and every host thread it touched would become a determinism leak.

---

## 2. Call protocol

```
guest code
  └─ MsgSend(...)                     shim: stateless forward, ENOSYS if absent
       └─ ops->msg_send(...)          bridge: fault, copy, trace
            └─ permute_ipc_send(...)  kernel: state transition, no blocking
            └─ if BLOCKED: park()     executor: baton to the next ready thread
            └─ on resume: result      errno written, return value returned
```

Rules:

| Rule | Enforcement |
|---|---|
| The shim reads the hypercall page, never a mutable global | `permute_ops()`, `tests` for the null-table path |
| A missing table or entry returns `-1` with `ENOSYS` | every shim function; `test_shim` |
| The bridge copies every payload before returning | `permute_ipc_send` copies into the kernel arena; the poison test overwrites the sender buffer after return |
| A blocking call parks exactly once | the return value of the kernel call decides; `permute_executor_park` asserts the baton |
| `errno` is per simulated thread | one host thread per guest thread; `errno` is written after resume |
| The kernel uses only the caller's stack | no kernel state is thread-local; `permute_kernel_t` is one global model |

`permute_executor_park()` is the only place the baton moves. `SchedYield` is
the explicit form; every blocking operation reaches the same function.

---

## 3. Operation table

`permute_kernel_ops_t` has 27 entries. All are required; a null entry is an
`ENOSYS` at the shim, not a fallback.

### 3.1 IPC — 13 operations

| Entry | Blocks? | Semantics |
|---|---|---|
| `channel_create(flags)` | no | allocates a channel; `_NTO_CHF_FIXED_PRIORITY`, `_NTO_CHF_UNBLOCK`, `_NTO_CHF_DISCONNECT` modelled |
| `channel_destroy(chid)` | no | wakes senders and receivers with `ESRCH`; emits disconnect pulses |
| `connect_attach(...)` | no | allocates a coid whose value equals the fd value |
| `connect_detach(coid)` | no | drops a reference; last detach fires `_NTO_CHF_DISCONNECT` |
| `msg_send(...)` | **yes** | copies the payload, queues by priority, parks in SEND/REPLY until reply, error, timeout, or death |
| `msg_receive(...)` | **yes** | dequeues a message or pulse; returns `0` for a pulse, positive rcvid for a message |
| `msg_reply(rcvid, ...)` | no | copies the reply into the client, invalidates the rcvid, wakes the client |
| `msg_error(rcvid, errno)` | no | wakes the client with `-1` and the error; invalidates the rcvid |
| `msg_send_pulse(...)` | no | queues a 64-byte pulse; a negative coid addresses the caller's own channel |
| `msg_receive_pulse(...)` | **yes** | waits for a pulse only; a short buffer returns `EFAULT` and drops the pulse |
| `msg_register_event(...)` | no | replaces the event with a handle scoped to the connection |
| `msg_deliver_event(...)` | no | fires a registered event; no priority boost |
| `msg_verify_event(...)` | no | answers whether the caller may fire this event |

### 3.2 Identity and time — 5 operations

| Entry | Blocks? | Semantics |
|---|---|---|
| `getpid()` | no | deterministic ordinal |
| `gettid()` | no | deterministic ordinal |
| `thread_token()` | no | 64-bit token, never a pointer |
| `clock_gettime(clock, ts)` | no | virtual time; never the host clock |
| `nanosleep(req, rem)` | **yes** | arms a timer and parks; virtual time advances only when nothing is runnable |

### 3.3 Entropy — 1 operation

| Entry | Blocks? | Semantics |
|---|---|---|
| `rand_seed()` | no | returns the per-process entropy origin derived from the run seed |

The shim expands the seed; libc's generator is never called.

### 3.4 Timers — 1 operation

| Entry | Blocks? | Semantics |
|---|---|---|
| `timer_timeout(clock, flags, event, ntime, otime)` | no (arms) | one-shot timeout for the next matching state; a non-matching state leaves it armed |

`TimerTimeout(ntime == NULL)` disarms and returns the previous deadline in
`otime` (FR-3.23; the target-vendor behaviour differs by design, see LIMITS #13).

### 3.5 Scheduling — 3 operations

| Entry | Blocks? | Semantics |
|---|---|---|
| `sched_set(pid, tid, policy, param)` | no | validates policy and priority, clears sticky inheritance |
| `sched_get(pid, tid, param)` | no | maps effective priority into `sched_priority` (IMP-52) |
| `sched_yield()` | **yes** | yields only to same-priority ready threads; lower priorities do not run (FR-3.19) |

### 3.6 Thread lifecycle — 4 operations

| Entry | Blocks? | Semantics |
|---|---|---|
| `pthread_create(token, attr, start, arg)` | no | creates a ready thread; the token is a 64-bit ordinal |
| `pthread_join(token, retval)` | **yes** | parks until the target exits |
| `pthread_exit(retval)` | — | terminates the calling thread; peers wake with `ESRCH` |
| `pthread_cancel(token)` | no | marks the target cancelled; it leaves at its next shim boundary (LIMITS #39) |

---

## 4. Object and identifier contract

| Rule | Detail |
|---|---|
| Handles are 64-bit | slot plus generation; a freed slot reused with a new generation rejects the stale handle |
| `rcvid_t` is 32-bit | matches the target interface; validity is a transition, not a guess |
| ids are ordinals | `pid`, `tid`, `coid`, `chid` are never host values or addresses |
| handles are per kernel instance | a new seed constructs a fresh kernel; no state survives |

The ready queue is 256 priority buckets with an occupancy bitmap, FIFO within a
priority. The bitmap and bucket occupancy are asserted in debug builds.

---

## 5. Fault contract

Faults are decided from `D_FAULT`, a decision domain of its own. That isolation
is structural, not a convention: adding a fault site cannot shift the
scheduling stream (FR-4.3).

| Site | Actions |
|---|---|
| `send` | drop, duplicate, reorder, delay, timeout, kill |
| `recv` | drop, duplicate, reorder, delay, timeout, kill |
| `reply` | drop, duplicate, reorder, delay, timeout, kill |
| `pulse` | drop, duplicate, reorder, delay, timeout, kill |

- An unarmed site draws nothing: instrumentation is free until a rule arms it.
- `DROP` at send models a message that never arrives. The sender is SEND-blocked
  with no message and only its own timeout can release it (IMP-53/54).
- `DUPLICATE` at receive is not modelled; `REORDER` is a bounded delay, not a
  queue reorder (LIMITS #41).
- `KILL` and `pthread_cancel` take effect at the next shim boundary, because a
  host thread cannot be stopped mid-instruction (LIMITS #43).

The plan is materialised from the seed, not stored: same seed and same rule list
produce the same fires in the same order.

---

## 6. Determinism contract

The kernel may not call:

- a real clock (`clock_gettime`, `gettimeofday`, `time`, `timer_*`);
- real entropy (`rand`, `random`, `getrandom`, `/dev/urandom`);
- host identity (`getpid`, `gettid`, `pthread_self` as a value);
- a hash container with observable iteration order.

`tools/symcheck.py` fails the build if a production C source names one of these
outside the shim file that exists to route it. The exception is allowlisted by
path so it is reviewable.

Every decision comes from `rnd(seed, domain, decision_index)`. Domains are
separate counters; adding a decision site in one domain cannot perturb another.
The trace records the decision value, and the digest covers the result, not the
wall clock.

Invariants that follow from the contract:

| Invariant | Test |
|---|---|
| Exactly one guest thread runs at any instant | executor `assert`; `test_executor` |
| A blocked thread is released only by the matching transition | `test_ipc`, `test_pulse`, `test_time` |
| A reply invalidates its rcvid exactly once | `test_ipc`; conformance C07/C09 |
| A receive-time priority is the maximum over pending messages | `test_pi`; conformance C12 |
| Virtual time advances only when the ready queue is empty | `test_time`; deadlock fuzz |
| An empty ready queue and empty timer heap is `OUTCOME_DEADLOCK`, never a hang | `test_cycle`; executor outcome |

---

## 7. Failure contract

| Condition | Behaviour |
|---|---|
| No operation table, or a null entry | `-1`, `errno = ENOSYS` |
| Unknown channel, coid, rcvid, or handle | `-1`, `errno = ESRCH` or `EINVAL` as the operation specifies |
| Unsupported event type, timer flag, or policy | `-1`, `EINVAL`/`ENOTSUP`; never approximated |
| Payload larger than the arena or a short receive buffer | `-1`, `EINVAL`/`EFAULT`; no truncation |
| Guest `fork`/`exec` | fails fast with `UNSUPPORTED_MULTIPROCESS` |
| Kernel internal invariant broken | abort in debug builds; the run is an internal failure, never a silent pass |

The rule behind the table: **fail loudly**. A silent approximation in this layer
would surface later as a false conformance result, which is worse than a
missing feature.

---

## 8. Adding an operation

1. Add the entry to `permute_kernel_ops_t` and its shim prototype.
2. Implement the semantic transition in `kernel/`, returning `Continue`/`Blocked`
   and a result; never touch the executor.
3. Implement the bridge op: fault decision, payload copy, trace record, park on
   `Blocked`, `errno` on return.
4. Add the entry to `g_ops` in `runtime/bridge.c`; a missing entry is `ENOSYS`.
5. Add a unit test for the normal, empty, and invalid case; add a conformance
   scenario if the target can observe it.

The ABI side of the same change is in [`ABI.md`](ABI.md) §7.
