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

### 2.2 Host harness — `permute-run`, `permute-worker`

```text
permute-kernel objects (libpermute_core.a)
  + executor backend (thread | fiber)
  + log codec + streaming oracles
  links no SUT objects; only dlopen()s process instances
```

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
| `SimMsg` | arena payload, reply buffer, sender pid/tid, send-time priority, sequence, `rcvid`, receiver tid, replied flag |
| `SimPulse` | `code`, `value` (never dereferenced), priority, source pid/tid, sequence |

Storage is arrays indexed by handle plus sorted vectors. Hash containers with observable iteration order are banned — iteration order is part of the observable trace.

Handles carry a generation counter so a stale handle is detected rather than silently reused. Receive IDs encode `(slot, generation)` and are always positive; `0` means "this was a pulse".

---

## 6. Scheduling `[specified]`

- Strict-priority preemptive, 256 buckets plus an occupancy bitmap. Pop is O(1); debug builds assert that the bitmap matches bucket occupancy.
- FIFO within a priority. `SCHED_FIFO`, `SCHED_RR`, `SCHED_OTHER` (treated as RR). RR quantum is four clock periods.
- `SchedYield` yields only to same-priority ready threads; with only lower-priority threads ready, it is a no-op.
- A higher-priority thread becoming ready preempts immediately.

### 6.1 Priority model

Two independent contributions, deliberately not merged:

| Term | Source | Reverts? |
|---|---|---|
| `sticky_prio` | message-driven inheritance: raised at send, assigned at receive | **No** — QNX semantics |
| `mutex_boost` | `PTHREAD_PRIO_INHERIT` mutex ownership | Yes, on unlock |
| `prot_ceil` | `PTHREAD_PRIO_PROTECT` ceiling of held mutexes | Yes, on unlock |

`eff_prio = max(base_prio, sticky_prio, mutex_boost, max(prot_ceil of held PROTECT mutexes))`, clamped to the process ceiling unless the process holds the priority ability.

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

---

## 7. Time `[specified]`

- `vtime_t` is `uint64_t` nanoseconds. No floating point anywhere in time handling.
- The virtual clock advances only when the ready queue is empty. It jumps to the earliest event, emitting everything due at that instant in `(time, insertion_seq)` order.
- An empty ready queue with an empty event heap is `OUTCOME_DEADLOCK`, never a hang.
- `TimerTimeout` arms a one-shot timeout for the next blocking state matching a mask bitfield. A timeout whose mask does not match the current state stays armed — this is a documented footgun and is modelled exactly.
- `TimerTimeout` with a null `ntime` disarms and returns the previous deadline.
- POSIX `timer_create`/`timer_settime` with pulse or signal notification map onto the same event heap.
- `nanosleep`, `clock_nanosleep`, and `delay` all route to the virtual clock.

---

## 8. Trace and log `[specified]`

Append-only binary `.pmt`, optimised for volume and salvage:

```text
Header  64 B : magic "PMT1" | version | flags | run_id | config_hash
               | sut_build_id | created_unix_ns
Chunk   64 KiB: hdr{first_seq, n_records, blob_len, prev_hash}
                records[] | blob[] | crc32c
Index   tail  : every 1024 chunks → (first_seq, file_offset, virtual_ns)
Footer  32 B  : n_records | n_chunks | final_hash | magic
```

| Field | Encoding |
|---|---|
| `kind` | one byte; 22 record kinds defined (channel, connection, send/receive/reply, pulse, event, timer, scheduler, block/unblock, sync, thread lifecycle, fault, oracle, stimulus, crash) |
| `seq` | delta varint |
| `dt_ns` | varint delta |
| `src`, `dst` | varint thread ordinals |
| `obj` | varint; channel / connection / timer / receive id per kind |
| `aux` | varint; errno, status, packed priority and state |
| payload | blob arena, length + offset as varints |

Average 20–24 bytes per record. A 50,000-event run is ~1.1 MB raw.

`run_digest = BLAKE3-64(hash_chain ‖ config_hash ‖ sut_build_id ‖ kernel_model_version)`.

Identical inputs produce an identical digest on any machine. Replay refuses on build, kernel, or ABI mismatch unless explicitly overridden.

Exports: NDJSON and Chrome Trace Format. Those are exports; they are never the source of truth.

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
