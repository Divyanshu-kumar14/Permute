<!-- SPDX-License-Identifier: Apache-2.0 -->

# Architecture

**Status:** design specification for v0.1. **No section is implemented yet.** Every heading is marked `[specified]` until the corresponding tests pass, at which point it flips to `[built]`; `[conditional]` marks work gated on an open decision. The document never claims more than the code does.
**Last updated:** 2026-11-14
**Companion documents:** [`DECISIONS.md`](DECISIONS.md) (why), [`LIMITS.md`](LIMITS.md) (what is not modelled), [`DETERMINISM.md`](DETERMINISM.md) (hazard-by-hazard proof obligations).

---

## 1. What Permute is

Permute is a deterministic simulation testing harness for QNX 8.0 message-passing systems.

It replaces the microkernel's IPC surface with a user-space model, runs a service graph against that model under a controlled scheduler, explores schedules by seed, and produces a reproducible failure artifact that another machine can replay from a seed alone.

Two properties define the design:

1. **The subject under test is rebuilt, not emulated.** The same C sources compile for QNX and for the simulator; only the link line changes. No privileged mode, no VM, no instruction translation.
2. **Determinism is a hard requirement, not an aspiration.** Two runs of the same seed, same build, same config produce byte-identical canonical traces.

```
                    one host process
  permute.toml ───► driver (sweep | replay | shrink | timeline)
                      │
                      │  dlopen(proc_*.so)
                      ▼
    ┌───────────────┬───────────────┬───────────────┐
    │ proc_srv__0   │ proc_cli__0   │ proc_drv__0   │   one .so per
    │   (server)    │   (client)    │   (driver)    │   process instance
    └───────┬───────┴───────┬───────┴───────┬───────┘
            │ MsgSend       │ MsgSendPulse  │
    ┌───────▼───────────────▼───────────────▼───────┐
    │ libpermute_guest.a   strong-symbol shim       │
    │ MsgSend / MsgReceive / MsgReply / pthread_* …  │
    └───────────────────┬───────────────────────────┘
                        │ direct call (no syscall)
    ┌───────────────────▼───────────────────────────┐
    │            SIMULATED MICROKERNEL              │
    │ objects │ channels │ msgs │ pulses │ PI        │
    │ ready queue (256 buckets + bitmap)            │
    │ virtual clock │ event heap │ rnd(seed,d,k)    │
    │ decision log │ trace emitter                  │
    └────────┬──────────────────────┬───────────────┘
             │ sched_switch()       │
      ┌──────▼──────┐        ┌──────▼──────┐
      │  fiber      │        │  thread     │
      │  (opt-in)   │        │  (default)  │
      └─────────────┘        └─────────────┘
                        │
              ┌─────────┴──────────┐
              ▼                    ▼
     .pmt decisions + trace    QNX 8.0 oracle
              │                 (capture agent)
              ▼                    │
          shrinker ──► case.pmt   │
              └──────► tracecmp ◄─┘
```

---

## 2. Link model `[specified]`

Three separate link products. Confusing them is the most common integration mistake.

### 2.1 SUT process instance — `proc_<name>__<n>.so`

```text
service objects (.c → .o)
  + libpermute_guest.a        ← strong-symbol shadowing (ADR-001)
  + libc                      ← never consulted for shadowed symbols
exports: permute_main
```

- `libpermute_guest.a` is linked **after** the service objects and **before** libc. The linker is single-pass, so the archive's definition wins.
- Each process instance gets its own `.so`. That is what gives genuinely separate `.data` and `.bss`; one merged binary would silently share statics and make conformance pass for the wrong reason.
- The instance is opened with `dlopen(..., RTLD_NOW | RTLD_LOCAL)` at a scheduled `EV_SPAWN` event, so static initialisers run in deterministic order.
- `dlmopen(LM_ID_NEWLM)` was considered and rejected (ADR-005). glibc's per-namespace libc copy is fragile, the namespace count is capped, and a fault there would present as simulator nondeterminism. Separate objects plus `RTLD_LOCAL` give the isolation with none of that.
- A version script (`guest/permute_guest.ld`, generated from `abi/permute_iface.def`) exports `permute_main` and the interposed ABI and hides everything else. The build checks it against the definition file and runs `nm -D` on the result, so the exported surface cannot drift and loader internals cannot leak.
- Each guest object is linked with `-Wl,-Bsymbolic-functions`. Without it, a `dlopen`ed object's own calls resolve through the global scope first, so libc's `clock_gettime`, `nanosleep`, `getpid`, and `rand` win over the shim's definitions. That failure is silent and would make a guest read the host clock. Binding intra-object calls to the object's own symbols is what makes the shim authoritative.
- Load and start are separate calls. Constructors run during load, so the executor controls order by loading in `EV_SPAWN` order; `permute_main` runs when the event is dispatched.

### 2.2 Host harness — `permute-run`, `permute-worker`

```text
permute-kernel objects (libpermute_core.a)
  + executor backend (thread | fiber)
  + log codec + streaming oracles
  links no SUT objects; only dlopen()s process instances
```

- `permute-run <seed> <trace>` executes exactly one seed and exits. SUT code only ever runs in this process, so a fault kills the runner and nothing else.
- `permute-worker` forks one runner per seed and classifies the wait status: exit 0 is `CLEAN`, death by signal is `CRASH`, a non-zero exit is a violation or an internal error, and an overrun past the wall budget is `BUDGET`. Budget is an outcome, not a failure (FR-5.6), so it does not change the exit code.
- Batches are claimed through lease files created with `O_EXCL`: the filesystem is the arbiter, so two workers cannot own the same batch. A lease whose recorded pid is gone is stale and can be reclaimed with `--steal`, which keeps one crashed worker from stranding a batch.
- The report is appended per seed (`report.jsonl`) and rewritten as a summary at the end. A killed worker still leaves usable evidence, and several workers can share one output directory.
- `--shard i/n` filters seeds by `seed % n == i` **before** batching, so a shard is reproducible regardless of how many workers run it.

### 2.3 Target twin — `permute-capture`, `scenarios/*`

Built with the vendor toolchain against real target headers. Links the same scenario sources as the simulator, plus the capture agent. Never links the host shim.

### 2.4 Interposition surface

Every interposed symbol is declared once in an X-macro:

```c
PERMUTE_SYMBOL(ipc,      long, MsgSend,   (int, const void *, size_t, void *, size_t), 0)
PERMUTE_SYMBOL(sched,    int,  SchedSet,  (pid_t, int, int, const struct sched_param *), 0)
PERMUTE_SYMBOL(pthread,  int,  pthread_mutex_lock, (pthread_mutex_t *), 0)
...
```

The macro generates prototypes, shim bodies, the symbol allowlist, and the audit manifest. Layer groups are atomic — `ipc`, `sched`, `pthread`, `time`, `proc`, `fs`, `stdio` — because shadowing part of a group (our mutex with libc's condvar) corrupts state. The build fails if a group resolves partially.

`tools/symcheck.py` enforces two things before anything is built:

- every symbol in the definition file is defined by `libpermute_guest.a`;
- no production C source calls a real clock, real entropy, host identity, or a hash container with observable iteration order. The shim files that exist to route those calls are allowlisted **by path**, so each exception is explicit and reviewable.

A mention inside a comment is documentation, not a call; the scanner strips comments first, preserving line numbers so diagnostics still point at the right line.

### 2.5 Deterministic allocator

The guest's `malloc` family is replaced by an arena allocator, so the address a program receives is a function of its allocation history rather than of libc's internal state.

| Property | Value |
|---|---|
| Reservation | 2 GiB, `PROT_NONE`, `MAP_NORESERVE`, at a fixed address (2 TiB) with a non-fixed fallback |
| Commit | 2 MiB windows, `mprotect`ed on first use; untouched reservation costs no RSS |
| Header | 16 bytes, carrying block offset, class, requested size, magic |
| Classes | 35 payload sizes, 16 B to 2 MiB; larger requests return `ENOMEM` rather than being approximated |
| Free lists | Per class, ordered by offset; allocation always takes the lowest-addressed free block |
| Reuse | Head, tail, and LIFO frees are O(1); a random free in the middle of a long list scans |

Addresses themselves never enter a trace record, a hash, or an ordering comparison (invariant 2). What is deterministic and observable is the **offset sequence**, which is what the 10⁶-operation test compares.

Pointers outside the arena belong to libc and are forwarded by the interposed `free`/`realloc`. A guest legitimately receives libc pointers from `getline`, `realpath`, and `dlopen`; treating those as arena blocks would corrupt both heaps.

The arena base is recorded, not assumed. If the fixed address is occupied the allocator falls back to an arbitrary mapping; offsets are unchanged, so a digest computed from offsets remains comparable, and the run manifest records the base that was actually used.

### 2.6 CLI

`permute` is the user surface. It owns configuration, scaffolding, and the exit-code contract; it never executes SUT code.

| Command | Behaviour |
|---|---|
| `init DIR` | Writes `permute.toml`, `scenarios/smoke.c`, `symbol-audit.txt`, `header-gaps.md`, `.gitignore`. Refuses a non-empty directory. |
| `build` | Compiles the smoke scenario into `.permute/build/proc_smoke__0.so` via `make_guest.py`; a failed compile exits 3. |
| `run` | Resolves the runner and worker, runs the configured seed range through the worker, writes `.permute/run/report.json` and `summary.json`. |
| `show` | Summarises a trace, or exports it as NDJSON or Chrome trace format. |
| `replay` | Delegates to the replay verifier (seed or artifact). |
| `clean` | Removes `.permute/`. |

Configuration is a **closed schema**: an unknown key is rejected with a `P####` code, a one-line cause, and a one-line fix. A typo in a kernel budget must not become a confusing result later.

Exit codes are the contract from PRD §9.5 and every command returns one: configuration problems are 2, a failed build 3, a refused replay 4, nondeterminism 5, a run-level deadline 6, a missing harness binary 7, a digest mismatch 11, and an interrupt 130. Codes whose real trigger is still staged (10, "violations differ") are asserted through a hidden harness command rather than left untested.

Before packaging exists, `tools/permute` is a launcher that sets `PERMUTE_HOME` and `PYTHONPATH` and runs the module. A packaged install provides the same entry point through a console script.

### 2.7 Scenarios

A scenario is a guest source exporting `permute_main`, compiled by `tools/make_guest.py` into `proc_<name>__0.so`. The same source is meant to build against real QNX headers on a target; the simulator supplies the ABI through `abi/permute_qnx.h`, which is hand-written from public documentation (ADR-011) and defines only what the modelled surface uses.

| Source | Workload |
|---|---|
| `scenarios/s01_hello` | 10,000 message round trips |
| `scenarios/s02_pi_chain` | client 10 → server 5 → driver 5; the driver must observe 10 |
| `scenarios/s03_timer_timeout` | every one of the eleven state-mask bits |
| `scenarios/s04_pulse_drop` | short `MsgReceivePulse` buffer: `EFAULT` and the pulse is lost |
| `scenarios/s06_unblock_pulse` | `_NTO_CHF_UNBLOCK`, timeout in `REPLY`, unblock pulse carrying the rcvid |
| `scenarios/s07_priority_ceiling` | `PTHREAD_PRIO_PROTECT` ceiling and refusal above it |
| `examples/telemetry` | one server, three clients at 5/15/25, pulses, timer, timeout, client kill |

`make scenarios` builds all of them and checks the two properties that compilation cannot show: the telemetry fanout names every modelled primitive, and s03 names all eleven mask bits. Those checks are static on purpose — see `LIMITS.md` #32 for why the scenarios are not yet executed end to end.

`abi/permute_qnx.h` deliberately does not include `<signal.h>`: glibc defines a different `struct sigevent` there, which would collide with the QNX-layout definition. A scenario that needs signal APIs is out of scope until the signal shims exist.

---

## 3. Kernel-as-library call protocol `[specified]`

There is no system call. The shim is a direct call into the kernel model running in the same address space.

```c
long MsgSend(int coid, const void *smsg, size_t sbytes, void *rmsg, size_t rbytes)
{
    permute_req_t req = { .op = OP_MSGSEND, .a0 = coid,
                          .p0 = smsg, .n0 = sbytes,
                          .p1 = rmsg, .n1 = rbytes };
    permute_rep_t rep;
    permute_enter(&req, &rep);              /* payload copied into the kernel arena */
    if (rep.action == PERMUTE_BLOCKED)
        permute_yield();                    /* the only place the baton moves */
    errno = rep.errno_v;                    /* restored on resume, per thread */
    return rep.ret;
}
```

Rules that make this sound:

| Rule | Reason |
|---|---|
| `enter()` copies every payload into the kernel arena | a SUT pointer must never be retained past the call; a debug poison test overwrites the sender buffer afterwards |
| the kernel runs on the calling guest thread's stack | the kernel is single-owner global state; no per-thread kernel context exists |
| `enter()` returns `Continue` or `Blocked` | one return value decides the whole scheduling question |
| only `permute_yield()` moves the baton | `assert(g_running_count == 1)` in debug builds |
| `errno` is written after resumption | with one OS thread per simulated thread, `errno` is genuinely per-thread |

Cost target: 60–150 ns for a non-blocking call, with an arena copy of ≤ 256 B.

### 3.1 The hypercall page

The operation table is published in a page mapped at a fixed virtual address, not in a mutable global.

| Property | Value |
|---|---|
| Reservation | 1 MiB, first page used |
| First candidate | 256 GiB (valid in a 39-bit user address space) |
| Later candidates | 1 TiB, 2 TiB, 128 GiB, 64 GiB |
| Mapping | `MAP_FIXED_NOREPLACE` — either the requested address or failure, never a silent clobber |
| Page contents | magic `PMTH`, version, size, pointer to `permute_kernel_ops_t` |

Two phases, because the page is shared but the *pointer to it* is not:

1. **Host**, once: `permute_hypercall_init()` tries the candidates in order, writes the magic, and binds the page in its own module.
2. **Guest module**, once per module: `permute_hypercall_bind(base)` validates the magic and adopts the page.

The second phase is not ceremony. Each simulated process is a separate shared object with its own statics, so a module that never binds sees a null page and fails every call with `ENOSYS`. Making the bind explicit keeps that failure visible instead of turning it into a mysterious null-pointer return.

If no candidate can be claimed, or `PERMUTE_NO_HYPERCALL` is set, the harness falls back to a module-local pointer, and `permute_hypercall_base()` returns 0 so the mode is reportable rather than guessed at.

Measured dispatch cost through the page: **3.1–3.6 ns** per shim call (10⁷ iterations, `-O2`), which is inside the 60–150 ns budget with the kernel work still to come.

---

## 4. Executor backends `[specified: thread default · fiber opt-in]`

### 4.1 Thread backend (default)

One OS thread per simulated thread, pre-spawned at boot. Handoff is a baton:

| Step | Outgoing thread | Incoming thread |
|---|---|---|
| 1 | computes `next = sched_pick()` | spins on `t->grant`, then parks |
| 2 | publishes `grant` and wakes `next` | observes the grant |
| 3 | parks on its own grant | clears the grant, becomes the running thread |
| 4 | resumes when granted again | executes guest code |

The futex is transport only. The **choice** of `next` is the scheduling decision; wake order must never influence anything. Spurious wakeups are handled by re-checking the grant in a loop.

Cost: 1.5–4 µs per switch. This backend exists because `errno`, `__thread`, `pthread_self`, and signal masks must behave as they do on the target.

### 4.2 Fiber backend (opt-in)

`ucontext` coroutines, 60–300 ns per switch, 4–8× throughput. Enabled only when `symcheck --strict` proves the SUT makes no real blocking syscall and touches no unvirtualised thread-local state. Published throughput always names the backend used.

---

## 5. Kernel object model `[specified]`

| Object | Key fields |
|---|---|
| `SimProcess` | `pid`, `name`, `dlopen` handle, `chids[64]`, `coids[1024]`, per-process RNG state, `prio_ceiling` (default 63), abilities |
| `SimThread` | `tid`, `pid`, `base_prio`, `sticky_prio`, `mutex_boost`, `prot_ceil`, `eff_prio`, `policy`, `state`, `cpu_ns`, `quantum_ns`, `wobj`, `rcvid`, one-shot timeout slot, host context |
| `SimConnection` | `coid`, client pid/tid, server pid/chid, flags, refcount |
| `SimChannel` | `chid`, owner pid, flags, receive queue head/tail, length, receive waiters, connection refs |
| `SimMsg` | arena payload, reply buffer, sender pid/tid, send-time priority, sequence, `rcvid`, receiver tid, replied flag, channel |
| `SimPulse` | `code`, `value` (never dereferenced), priority, `scoid`, source pid/tid, sequence |

Storage is arrays indexed by handle plus sorted vectors. Hash containers with observable iteration order are banned — iteration order is part of the observable trace.

**Handles** are 64 bits: slot in the low 32, a 32-bit generation in the high 32. Freeing a slot increments its generation, so a stale handle is rejected instead of silently addressing whatever object reuses that slot. Thirty-two bits of generation means the counter cannot wrap inside a realistic run, which is what "generation never repeats" actually requires.

**Receive IDs** are a separate encoding because the ABI forces a positive `int32`: slot + 1 in the low 16 bits, generation in the high 15. The sign bit stays clear, `0` means "this was a pulse", and the generation wraps at 32768 — a documented aliasing bound, not a hidden one. The message pool *is* the rcvid space, so there is one generation source and no parallel index that can disagree.

### 5.1 Pulses and events `[specified]`

A pulse is the fixed 24-byte `struct _pulse` on LP64: `type`, `subtype`, an 8-bit `code`, an 8-byte `value`, and a `scoid`. The code is an `int8_t` on the wire, so codes above 127 read back sign-extended; the documented safe range is `0..127`.

Pulses share the channel queue with messages and are ordered by priority descending, then arrival. A pulse never blocks its sender and never changes the receiver's priority.

Three rules that are not obvious:

| Rule | Behaviour |
|---|---|
| Local addressing | A negative coid means `-chid` in the **caller's own** process, so a thread can signal itself without a connection. `scoid` is 0 for such a pulse |
| `MsgReceivePulse` short buffer | A buffer below 24 bytes returns `EFAULT` and the pulse is **lost** — including when the thread blocked first and lost the pulse that woke it. Unlike `MsgReceive`, there is no truncation |
| Waiter eligibility | A pulse goes to the highest-priority waiter that accepts a pulse (either kind); a message only to one that accepts messages. A pulse-only waiter therefore never consumes a message, and a queued message never blocks a pulse from reaching it |

**Secure events** follow the vendor mechanism rather than a byte comparison. `MsgRegisterEvent` replaces the caller's `sigevent` with an opaque handle and records the single server permitted to fire it; `MsgDeliverEvent` and `MsgVerifyEvent` check handle ownership against the message's sender. The normalised event is 32 bytes and layout-compatible with the platform struct, with padding excluded from comparison because those words are unspecified.

Event delivery posts a pulse and touches nothing else: **it does not raise the target's priority**. Sticky priority is a message-driven effect.

**Disconnect notification.** A channel created with `_NTO_CHF_DISCONNECT` receives `_PULSE_CODE_DISCONNECT` (value and `scoid` = the connection id) when a client drops its *last* connection to it. A process death detaches the same way, so the server sees the notification rather than a silent disappearance.

---

## 6. Scheduling `[specified]`

- Strict-priority preemptive, 256 buckets indexed by effective priority, plus a 256-bit occupancy bitmap. Pop is O(1): the highest non-zero bitmap word in at most four iterations, one `clz` for the bit, then that bucket's head. Debug builds assert that the bitmap, counts, links, and total agree.
- FIFO within a priority, on intrusive doubly-linked lists indexed by thread slot. Insertion at head or tail and removal from the middle are all O(1); nothing scans the thread table and nothing allocates.
- A thread that becomes ready at a higher priority goes to the **head** of its bucket; an RR expiry and a yield go to the **tail**.
- `SCHED_FIFO` never loses the CPU on time. `SCHED_RR` and `SCHED_OTHER` (an alias for RR) expire after four clock periods; the modelled period is 1 ms.
- On expiry the thread is requeued at the tail and the scheduler picks again. If no equal-priority peer is ready, the same thread wins and its quantum restarts — a lone RR thread does not produce phantom switches.
- `SchedYield` is implemented as "push self at the tail of its own bucket, then pick". That yields to an equal-priority peer for free and can never yield to a lower priority, because the pick starts at the highest non-empty bucket, which is self's. No special case, no scan (FR-3.19).
- The running thread is never in the ready queue. `permute_sched_check` enforces that separately from the queue's internal invariant.

**Boundary.** Thread creation and destruction maintain ready-queue membership, and a priority change re-places a queued thread. IPC state transitions (`SEND`, `RECEIVE`, `REPLY`) do **not** move threads in or out of the queue yet: that is the executor's job, which arrives with the run loop. Until then, queue membership is driven explicitly by the caller, and the unit tests do exactly that.

### 6.1 Priority model

Three independent contributions, deliberately not merged:

| Term | Source | Reverts? |
|---|---|---|
| `sticky_prio` | message-driven inheritance: raised at send, assigned at receive | **No** — not on reply, not when the client dies |
| `mutex_boost` | `PTHREAD_PRIO_INHERIT` mutex ownership | Yes, on unlock |
| `prot_ceil` | `PTHREAD_PRIO_PROTECT` ceiling of held mutexes | Yes, on unlock |

`eff_prio = max(base_prio, sticky_prio, mutex_boost, prot_ceil)`, clamped to the process ceiling unless the process holds the priority ability.

**Message-driven, step by step:**

1. **At send**, if a thread is already receive-blocked on the target channel and the sender is higher, that thread is raised to the sender's priority *before* it runs. Without this, a medium-priority thread can delay the receive.
2. **At receive**, the inherited value is **assigned** — not raised — to the maximum over everything still pending on the channel plus the item just dequeued. A lower-priority sender therefore lowers the inherited value.
3. **No revert on reply.** The value stays until the next receive, an explicit scheduling call, or a higher-priority send.
4. **Multi-hop chains** work without a donor list: a server forwards at its inherited priority, so the next hop inherits from it.
5. `_NTO_CHF_FIXED_PRIORITY` disables both steps for that channel.
6. Donations are clamped to the process ceiling unless the process holds `PROCMGR_AID_PRIORITY`.

**Mutex inheritance** is classic and reverting. A `PTHREAD_PRIO_INHERIT` owner is raised to the highest blocked waiter, including transitively through a wait chain, and the boost is recomputed from the remaining waiters on unlock. `PTHREAD_PRIO_PROTECT` instead raises the locker to the mutex ceiling for as long as it holds it, and refuses a locker already above that ceiling. Unlock hands the mutex to the highest-priority waiter directly.

**One documented divergence.** The vendor text says a receive from a *lower*-priority sender lowers the server's effective priority below its own base (a server configured at 22 serving a priority-10 client runs at 10). PRD 9.1 defines effective priority as a maximum including `base_prio`, so in this model the inherited value becomes 10 while the effective priority stays 22. The PRD formula is normative here; the difference only exists when a receiver's own priority exceeds its client's, and it is recorded in [`LIMITS.md`](LIMITS.md).

The non-reverting behaviour is the one reviewers ask about first. It is intentional, it is pinned by a conformance scenario, and a mutation that reintroduces revert-on-reply must be caught by the test suite.

### 6.2 Thread states

`RUNNING`, `READY`, `SEND`, `REPLY`, `RECEIVE`, `NANOSLEEP`, `MUTEX`, `COND`, `SEM`, `JOIN`, `INTR`, `STOPPED`, `ZOMBIE`, `DEAD`.

The transitions that are easy to get wrong:

| From | Event | Guard | To | Action |
|---|---|---|---|---|
| RUNNING | `MsgReceive` | queue empty | RECEIVE | enqueue waiter; arm timeout if the mask includes receive |
| RUNNING | `MsgReply` | valid rcvid | RUNNING | copy out; client → READY; **server priority unchanged** |
| any | timeout fires | state ∈ mask | RUNNING | unblock with `ETIMEDOUT`; disarm |
| any | timeout fires | state ∉ mask | unchanged | **stay armed** — the classic QNX footgun |
| REPLY | sender cancelled | channel has `_NTO_CHF_UNBLOCK` | READY | emit unblock pulse carrying the rcvid |
| RUNNING | quantum expiry | policy ∈ {RR, OTHER} | READY | tail of bucket; reset quantum |
| READY | higher-priority thread becomes ready | `prio > eff_prio` | READY | head of own bucket |
| any | `SchedSet` | — | unchanged | set base, **clear sticky**, requeue at head |

Two ordering rules that are easy to conflate:

- A sender is in `SEND` while its message sits on the channel, and moves to `REPLY` the moment a server **receives** it — not when the server replies.
- A channel's receive queue is ordered by send priority descending, then insertion sequence ascending; a thread blocked in `MsgReceive` is chosen by the same rule. Equal-priority traffic therefore stays FIFO.

---

## 7. Time `[specified]`

- `vtime_t` is `uint64_t` nanoseconds. No floating point anywhere in time handling.
- The virtual clock advances only when nothing is runnable. It jumps to the earliest live deadline, emitting everything due at that instant in `(deadline, sequence)` order.
- An empty ready queue with an empty heap is `OUTCOME_DEADLOCK`, never a hang.
- Thread timeouts and POSIX timers share one absolute-time binary heap, so ordering between them is total. Entries are invalidated lazily by sequence number; a superseded entry is dropped **before** the clock moves to its deadline, or replacing a timer would still be observable as a time jump.
- `TimerTimeout` stores an intent first. The entry enters the heap only when the thread actually blocks in a state the mask covers, which is what makes the non-matching case stay armed (FR-3.22).
- Disarm is `flags == 0` (the vendor's rule) or `ntime == NULL` (PRD 3.23). Either way the previous deadline comes back in `otime`. QNX 8.0 makes `ntime == NULL` an immediate timeout instead; see conflict C-09.
- On expiry the blocked call fails with `ETIMEDOUT`, with one exception: a client in `REPLY` on a channel with `_NTO_CHF_UNBLOCK` stays blocked and the server receives `_PULSE_CODE_UNBLOCK` carrying the rcvid. The server decides when to reply.
- A timed-out `SEND` destroys its queued message; a timed-out `REPLY` destroys the message so a late reply gets `ESRCH`; a timed-out `MUTEX` waiter is removed and the owner's boost recomputed.
- POSIX `timer_create`/`timer_settime` accept pulse and unblock notification. Signal notification is refused with `ENOTSUP` rather than approximated.
- `nanosleep`, `clock_nanosleep`, and `delay` route to the same clock (the blocking call is wired in a later task; the state bit and timeout plumbing already exist).

---

## 8. Trace and log `[specified]`

Append-only binary `.pmt`, optimised for volume and salvage. All integers are
little-endian and written field by field, so the format does not depend on
struct layout.

```text
Header 72 B  : magic "PMT1" | version u16 | flags u16 | run_id[16]
               | config_hash u64 | sut_build_id[32] | created_unix_ns u64
Chunk 64 KiB : hdr{ first_seq u32 | n_records u16 | blob_len u16
               | prev_hash u64 } | records[] | free | blobs[] | crc32c u32
Index tail   : every 1024 chunks -> { first_seq u32 | offset u64 | vtime u64 }
Footer 32 B  : n_records u64 | n_chunks u32 | final_hash u64 | magic u32
               | reserved u64
```

The header is **72 bytes**, not the 64 the prose says: its own field list does
not fit in 64, and the fields are authoritative (conflict C-11). `first_seq`
is 32 bits, so a single run is limited to 2³² records by the format.

| Field | Encoding |
|---|---|
| `kind` + flags | one byte, `kind << 3 | flags`; flags are `BLOB` and `EVENT` |
| `seq`, `dt_ns` | varint deltas from the previous record, across chunk boundaries |
| `src`, `dst` | varint thread ordinals; `0xFFFF` means kernel or external |
| `obj`, `aux` | varint; channel / connection / rcvid / timer, and errno/status/mask |
| payload | `blob_len`, `blob_off` as varints; the blob itself at the back of the chunk |
| event | 32 bytes inline, for `TIMER_ARM` and `EVENT_DELIVER` |

Which optional fields a record carries is a **table**, not a convention: the
`schema` array in `kernel/trace.h` (mirrored in `cli/permute/trace/codec.py`)
is the authoritative definition. Prose summaries of the fields are not.

**Integrity.** Each chunk is hashed as `BLAKE3-64(prev_hash ‖ chunk[0..65532))`,
so the chain covers every byte that precedes the next chunk's pointer. The CRC
covers the same range and is checked first. The final hash goes in the footer
and is the head of the chain.

**Damage.** A truncated file is a normal outcome of a crashed run, not an
error. The reader recovers every complete chunk and the records that fit in a
partial one, sets `partial`, and never raises. A CRC mismatch or a broken link
sets `corrupt` / `chain_ok = False` and stops at the first bad chunk; records
before it are still returned. Blobs in a partial chunk are unrecoverable
because the blob area is anchored at the chunk's far end, so those records
carry `blob = None`.

`run_digest = BLAKE3-64(final_hash ‖ config_hash ‖ sut_build_id ‖ kernel_model_version)`.
`created_unix_ns` is excluded, so two runs of the same seed agree byte for byte
apart from a field the digest does not cover.

Identical inputs produce an identical digest on any machine. Replay refuses on
build, kernel, or ABI mismatch unless explicitly overridden.

Exports: NDJSON and Chrome Trace Format. Those are exports; they are never the
source of truth, and both carry the digest so an exported artifact can be tied
back to its run.

The reader currently scans chunks linearly; the index is written and reserved
for seeking, which the `show` command will use once traces outgrow a linear
read.

---

## 9. Exploration, replay, shrinking `[specified]`

```
seed → run → decisions + trace
                 │
                 ├─ oracle violation ──► artifact (seed, tape, faults, trace, cone)
                 │                              │
                 │                              ▼
                 │                        shrink (inputs only)
                 │                              │
                 │                              ▼
                 │                        case.pmt + minimality
                 │
                 └─ novelty feature map ──► corpus ──► mutate ──► next seed
```

- **Search policy.** Novelty-guided mutation over executed tapes, with features over decision-state and causal edges. Random remains available and is the control condition for the claim that novelty helps.
- **Shrinking.** Causal pre-slice → stimulus ddmin → time-window minimisation → tape elimination → fault materialisation. Budget: 1,500 executions or 90 s. Deterministic: no RNG, fixed iteration order, byte-identical output on repeat.
- **Replay.** Authoritative from the artifact, and also possible from the seed alone. Exit codes distinguish "violations differ" from "violations match but the log digest differs".

### 9.1 Replay contract

`permute replay` has two modes:

| Mode | What it proves | Exit codes |
|---|---|---|
| `replay seed N` | the same seed produces the same digest in separate processes | 0 verified, 5 nondeterminism, 7 fixture failure |
| `replay artifact F` | the trace is intact, its ordering is consistent, its manifest matches, and its digest re-derives | 0 verified, 2 unusable artifact, 4 manifest mismatch, 11 digest mismatch |

The manifest is compared against values the caller supplies (`--build-id`,
`--config-hash`, `--kernel`, `--digest`), not against the artifact's own
claims: a file that lies about its build id must not be able to authorize
itself. `--force` accepts a build/config/kernel mismatch; it does **not**
accept a digest mismatch, because that check is the authoritative one.

`verify_ordering` checks what can be proved without understanding events:
`seq` strictly increasing, `t_ns` non-decreasing, every record's optional
fields present exactly when its kind's schema says they are.

A partial trace is refused for replay (exit 2). A partial trace is fine to
read for diagnosis, but it cannot prove what a run did.

**Staged.** Exit 10 ("violations differ") and exit 1 ("violation found")
require the oracle engine, which is a later task. Replay currently proves
integrity, manifest, ordering, and digest, and its output says so rather than
implying the violation check ran.

Until the run loop exists, "from seed" is exercised by
`tools/replay_fixture`, a deterministic driver that executes a seeded
send/receive/reply/pulse workload through the kernel and writes a trace. It is
the same mechanism the run loop will use, without the scenario loader.

---

## 10. Oracles `[specified]`

All oracles are post-hoc streaming predicates over the event log, so the *same code* runs against simulator traces and captured target traces.

`lost_pulse`, `deadlock` (with the wait-for cycle), `unbounded_block`, `priority_inversion` (distinguishing "inheritance not configured" from "inheritance not applied"), `starvation` (only when no higher-priority task was ready during the gap), `orphan_reply`, `queue_depth`.

Each oracle is configurable per scenario with severity `error` or `warn`. Unknown configuration keys are rejected.

---

## 11. Determinism invariants `[specified]`

Every PR is reviewed against these. A violation is a blocking defect.

| # | Invariant |
|---|---|
| 1 | Exactly one guest thread runs at any instant |
| 2 | No pointer value enters a trace record, an ordering comparison, or a hash |
| 3 | No floating point in kernel state, timers, or ordering |
| 4 | No container with observable iteration order |
| 5 | All time APIs route to the virtual clock |
| 6 | All entropy routes to `rnd(seed, domain, decision_index)` |
| 7 | No real signals in guest workers |
| 8 | Locale and timezone fixed before the first guest instruction |
| 9 | Static initialisers run at scheduled spawn events |
| 10 | Separate `.data`/`.bss` per process instance |
| 11 | Identifiers are small integers or spawn-order ordinals |
| 12 | The log is append-only, checksummed, and truncation-tolerant |
| 13 | Oracles are post-hoc and target-agnostic |
| 14 | The shrinker never mutates events and contains no RNG |
| 15 | A seed depends only on `(seed, resolved config, SUT build)` |

The proof obligations and the test that discharges each one are in [`DETERMINISM.md`](DETERMINISM.md).

---

## 12. Repository map `[specified]`

```text
kernel/      simulated microkernel: objects, sched, PI, IPC, timers, trace
abi/         interposition: iface.def, shims, allocator, allowlist
guest/       per-process entry point, hypercall page, linker script
backends/    thread and fiber executors
dispatch/    dispatch/resmgr/iofunc subset          [conditional]
cli/         Python CLI, config, reports, shrink, tracecmp, explore
worker/      seed batching, leases, crash isolation
scenarios/   conformance and example programs
conformance/ runner, oracle traces, trace comparison
qnx/         target build and image recipe
tests/       unit, golden, mutation, fixtures
docs/        this document and its companions
```

### 12.1 One seed, end to end

1. The CLI resolves configuration, computes `config_hash`, writes a run manifest.
2. A worker claims a batch of seeds from an atomic chunk index.
3. For each seed, `permute-run` builds a fresh kernel instance. No state survives.
4. Process instances are `dlopen`ed in scenario order at `EV_SPAWN`.
5. The scheduler picks a runnable thread; each kernel call returns `Continue` or `Blocked`.
6. Decision sites draw from `rnd(seed, domain, index)` and append to the decision log.
7. Kernel transitions emit binary records.
8. On exit the trace is flushed, chunk-hashed, and `run_digest` is computed.
9. Oracles stream over the trace; violations become failure artifacts.
10. Failures are shrunk to a minimal input set.

---

## 13. Conformance `[specified]`

The fidelity claim rests on four tests, not on assertion:

| Test | Method | Pass criterion |
|---|---|---|
| Self-determinism | same seed, repeated runs, both backends, ASLR on and off | byte-identical canonical traces |
| Cross-target | 12 mandatory scenarios on simulator and target | ≥10/12 strict trace equivalence, 12/12 outcome-class |
| Negative control | intentionally racy scenario on the target | traces **must differ**; if they do not, the oracle cannot detect nondeterminism and the job fails |
| Model mutation | 12 seeded kernel defects | ≥10/12 killed |

Canonical traces normalise identifiers, replace payloads with digests, and compare causal order, not wall-clock order. Allowed differences are enumerated in [`LIMITS.md`](LIMITS.md) §4 and, once capture is performed, in `conformance/ALLOWED-DIFFERENCES.md`.
