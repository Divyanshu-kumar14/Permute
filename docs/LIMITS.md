<!-- SPDX-License-Identifier: Apache-2.0 -->

# Limits

**Status:** applies to v0.1. Every claim below is either verified by a committed test or explicitly marked unverified. Nothing here is aspirational.
**Last updated:** 2026-11-14
**Companion documents:** [`ARCHITECTURE.md`](ARCHITECTURE.md) (how it works), [`DECISIONS.md`](DECISIONS.md) (why), `conformance/ALLOWED-DIFFERENCES.md` (per-field diffs observed at capture time).

If this file says a behaviour is modelled, there is a test that pins it. If it says a behaviour is not modelled, the simulator must fail loudly rather than silently approximate it.

---

## 1. What Permute is not

| Question | Answer |
|---|---|
| Is this a QNX emulator? | No. It models the IPC and scheduling semantics that message-passing services depend on. It does not execute target binaries. |
| Does it run my existing binary? | No. Rebuild against the shim. One flag or link line; no source changes. |
| Why not just use QEMU? | QEMU gives you one nondeterministic execution. This gives you a controlled schedule space, replay from a seed, and automatic minimisation. |
| Is it real-time? | No. Time is virtual and advances only when the ready queue is empty. Latency and jitter measurements are meaningless here. |
| Does it prove absence of bugs? | No. It explores schedules and checks invariants. It is evidence of exercised paths, not a proof of correctness. |
| Does it need a QNX licence? | Not for the simulator. A target toolchain is needed only to build the capture agent and the target side of conformance. |

---

## 2. Modelled surface

The interposition surface is 41 symbols across seven layer groups. Groups are atomic: a partially shadowed group fails the build, because mixing our primitives with libc's corrupts state.

| Group | Modelled | Notes |
|---|---|---|
| `ipc` | `ChannelCreate/Destroy`, `ConnectAttach/Detach`, `MsgSend`, `MsgSendv`, `MsgReceive`, `MsgReceivev`, `MsgReceivePulse`, `MsgReply`, `MsgReplyv`, `MsgError`, `MsgInfo`, `MsgRead`, `MsgWrite`, `MsgSendPulse`, `MsgSendPulsePtr`, `MsgDeliverEvent`, `MsgRegisterEvent`, `MsgVerifyEvent` | Pulse vs message returns, short-buffer `EFAULT`, negative connection id addressing the local channel, receive queue ordering by priority then insertion, unsent message to a destroyed channel returning `ESRCH` |
| `sched` | `SchedSet`, `SchedGet`, `SchedYield`, `SyncMutexLock/Unlock`, `SyncTypeCreate` | Message-driven inheritance is sticky and does not revert on reply; mutex inheritance is classic and reverting |
| `pthread` | `pthread_create/join/exit/detach`, mutex family including `PTHREAD_PRIO_INHERIT` and `PTHREAD_PRIO_PROTECT`, cond, rwlock, sem, once, barrier | Waiters are priority-ordered; cond wait releases, blocks, and reacquires as one atomic simulation step |
| `time` | `TimerTimeout`, `timer_create`, `timer_settime`, `nanosleep`, `clock_nanosleep`, `delay` | One-shot timeouts selected by a state-mask bitfield; a non-matching state leaves the timeout armed |
| `proc` | `getpid`, `gettid`, `pthread_self`, `exit`, `_exit`, `abort`, `atexit`, `__cxa_atexit` | Identifiers are deterministic ordinals; exit handlers run LIFO and peers wake with `ESRCH` |
| `fs` | `open`, `read`, `write`, `close`, `readdir` | Read-only from a scenario manifest; writes to undeclared paths return `EROFS` and emit a trace record |
| `stdio` | `printf` family | Deterministic formatting; `LC_ALL=C` and `TZ=UTC` are forced before the first guest instruction |

### 2.1 Semantics that are modelled exactly (and often got wrong by tools)

- `MsgReceive` returns `0` for a pulse and a positive receive id for a message.
- The connection id equals the file-descriptor value, matching the target's unification.
- A receive-time priority assignment takes the maximum over messages still pending on that channel, including the one being dequeued.
- A timeout whose state mask does not match the current blocking state stays armed.
- `MsgDeliverEvent` requires prior registration and does **not** boost priority.
- A negative connection id in `MsgSendPulse` addresses the caller's own channel.
- A pulse buffer smaller than the pulse structure returns `EFAULT` and the pulse is dropped — including when the thread blocked before the pulse arrived, in which case the wake-up itself is lost.
- `MsgRegisterEvent` replaces the caller's event with a handle, and only the connection's server may fire it.
- `_NTO_CHF_DISCONNECT` fires on the client's last detach from a channel, including a detach performed by the kernel when the client dies.
- A receive-blocked server is raised to a higher-priority sender's priority at send time, then assigned the maximum over pending items at receive; replying never reverts it.
- `_NTO_CHF_FIXED_PRIORITY` disables message-driven inheritance for that channel.
- `PTHREAD_PRIO_INHERIT` boosts an owner to its highest blocked waiter, transitively and reversibly; `PTHREAD_PRIO_PROTECT` raises the locker to the mutex ceiling and refuses a locker above it.

### 2.2 Dispatching and resource managers `[conditional]`

A small dispatch and resource-manager subset is implemented only if a timeboxed spike proves the canonical resource-manager skeleton runs unmodified. See D-04 in [`DECISIONS.md`](DECISIONS.md).

If that spike fails, the following are **not** modelled and are recorded as a known gap:

- `dispatch_create`, `dispatch_context_alloc`, `dispatch_block`, `dispatch_handler`
- `resmgr_attach` and the longest-prefix pathname namespace
- `iofunc_*` (attributes, open control blocks, default handlers)
- `name_attach` / `name_open` beyond the simple global name table

---

## 3. Not modelled

| Area | Why |
|---|---|
| Unmodified target binaries | Would require a documented kernel-trap ABI; version-specific and privileged |
| SMP, runmasks, processor clustering | Deterministic multi-core scheduling is a research project; removed as a category by modelling one CPU |
| Interrupt latency, cache effects, memory bandwidth | No hardware model; interrupt-driven paths are stubbed |
| Multi-process via `fork`/`exec` | A service graph is one binary with N threads linked against the shim; `fork` fails fast with `UNSUPPORTED_MULTIPROCESS` |
| Full kernel, drivers, networking (`io-pkt`, `devb-*`, filesystems) | Years of work, no additional evidence for the IPC claim |
| Instruction-level data races inside one process | Requires a sanitiser on hardware; out of scope |
| Adaptive partitioning (`SchedCtl(SCHED_CTL_APS_*)`) | Removed from the target version being modelled |
| Cancellation points | Cancellation points are not modelled |
| `SIGEV_SIGNAL`, `SIGEV_SIGNAL_CODE`, `SIGEV_SIGNAL_THREAD`, `SIGEV_THREAD`, `SIGEV_MEMORY` events | Signals are not modelled; `MsgRegisterEvent` rejects them with `EINVAL` and `timer_create` with `ENOTSUP`, rather than approximating |
| Timer tolerance (`TIMER_TOLERANCE`) | No jitter model exists; the flag is rejected rather than silently ignored |
| `_NTO_REGEVENT_ALLOW_ANY_SERVER`, `_NTO_REGEVENT_ALLOW_SELF`, `SYSMGR_COID` registration targets | Their numeric values are not given in the library reference; inventing constants would create a fake ABI. Registration requires a real connection |
| `_NTO_CHF_COID_DISCONNECT` (client-side death notification) | Needs per-connection tracking on the client's own channel; no requirement pins it yet |
| `_NTO_CHF_THREAD_DEATH` | The reference does not say which channel the pulse lands on |
| Per-channel pulse pools (`ChannelCreatePulsePool`) | Pulse allocation is a single global pool; exhaustion is `EAGAIN` either way |
| Security keys (`MsgKeyData`, `MsgKeyUnlock`) | Not modelled |
| Signals asynchronously interrupting guest code | Signals are *scheduled events*, not host signals; ordering against pulses is therefore not modelled |
| Version 7.1 / 6.5 | Semantics differ in ways this model cannot express |
| Graphics, audio, USB, PCI | No driver model |
| Live dashboard or GUI | Report is a static, self-contained HTML file plus machine-readable formats |
| Plugin system or scenario DSL | Abstraction tax before the product exists |
| Distributed / multi-machine simulation | Out of scope |
| Record and replay on real hardware | Needs trace hooks in the target kernel |

---

## 4. Known and allowed divergences

A conformance comparison tolerates differences only where the target itself does not specify the behaviour. Each entry below is a reason, not an excuse.

| # | Divergence | Reason |
|---|---|---|
| 1 | Wall-clock and virtual timestamps | The simulator uses virtual time; the target has tick jitter. Only ordering is compared. |
| 2 | Numeric identifiers | Allocation order is implementation-dependent. Identifiers are normalised before comparison. |
| 3 | Order of independent events with no causal path | The target does not specify equal-priority tie-breaks. Comparison uses a canonical linear extension. |
| 4 | Context-switch and preemption counts | Not semantic; counters differ without any behavioural difference. |
| 5 | Absolute latency and throughput | Target runs under emulation; numbers say nothing about the model. |
| 6 | Pulse coalescing or compression, if it occurs | QNX 8.0 documents compression of identical consecutive pulses at one priority. It cannot reorder delivery or change what a receiver observes, so it is unmodelled by design (IMP-16) |
| 7 | Startup order before the trace barrier | Initialisation before the measurement window is not part of the claim. |
| 8 | Error code when two errors race (e.g. timeout vs peer death) | Both outcomes are legal; comparison uses an allowed set. |
| 9 | Memory addresses, stack usage, ASLR effects | The simulator never exposes addresses; the target does. |
| 10 | Signal-versus-pulse ordering | Signals are not modelled as asynchronous interrupts. |
| 11 | A receive from a lower-priority sender: effective priority stays at the receiver's base | The vendor text says the inherited priority can fall below the thread's own priority. PRD 9.1 defines `eff_prio` as a maximum that includes `base_prio`, and the PRD is normative. The inherited value is still assigned (and observable), only the effective priority is floored. No conformance scenario exercises a receiver configured above its clients |
| 12 | Unprivileged priority ceiling is 63 | The target defaults to unprivileged priorities 1–62 with 63 as the first privileged level (`procnto -P`). The PRD fixes the modelled ceiling at 63 and all donation tests are written against that ceiling |
| 13 | `TimerTimeout(ntime == NULL)` disarms | QNX 8.0 treats it as an immediate timeout (polling); `flags == 0` is the vendor's disarm. The PRD is explicit (FR-3.23), so the kernel follows it and the shim must translate the polling idiom to a zero-length timeout. See C-09 |
| 14 | An armed timeout survives a non-matching blocking call | QNX 8.0 disarms on exit from the next kernel call whatever state it blocked in. FR-3.21/3.22 require the latch. The two models agree whenever the timeout is armed immediately before the call being timed, which is the documented usage. See C-10 |
| 15 | Blobs in a partially written chunk are unrecoverable | The blob area is anchored at the chunk's far end, which a truncated file does not contain. Records are still recovered; their `blob` is `None`. A complete chunk always recovers its blobs |
| 16 | The trace index is written but not used by the reader | A linear scan is correct and adequate at current sizes; the index exists for `show`/seek once traces grow. Writing it now keeps the format stable |
| 17 | Replay does not yet compare violations | The oracle engine is a later task. Exit 10 and exit 1 are therefore unreachable today; replay proves integrity, manifest, ordering, and digest, and says so in its output rather than implying the full FR-6.4 check ran |
| 18 | `EV_SPAWN` is not yet a scheduled event | The loader splits load (constructors) from start (`permute_main`) so the executor can place each half at the event, but the event loop does not exist. The test proves constructor order equals load order, which is the property the event will rely on |
| 19 | Per-thread TLS across guest instances is not yet tested | Separate objects give each instance its own TLS by construction, but the fixture is single-threaded. The per-thread case arrives with the executor and the pthread shim |
| 20 | Allocations above 2 MiB return `ENOMEM` | The largest size class is 2 MiB. Forwarding large requests to libc would reintroduce allocator dependence; refusing is loud and documented |
| 21 | A free in the middle of a long free list scans | Head, tail, and LIFO frees are O(1); a random middle insertion is O(n) in the list length. No observable behaviour depends on it. A tree can replace the list without changing the contract |
| 22 | RSS is not measurable under ASan | ASan's shadow mapping dominates resident memory, so the 70 MB check is skipped in the sanitizer build and enforced in the normal build and the interposed probe |
| 23 | `pthread_create` is not shimmed yet | It needs the executor; a shim returning a host thread would reintroduce real scheduling. `pthread_self` and `pthread_equal` are modelled |
| 24 | Signals (FR-4.10) are not yet interposed | `sigprocmask`/`sigaction`/`kill` need the scheduled-event model. The gate prevents real signal APIs appearing in production code, but the shims land with the executor |
| 25 | `rand` without a kernel table uses seed 0 | `rand` cannot report an error. Outside a run this is deterministic but meaningless; inside a run the host always installs the table. The determinism gate keeps real entropy out of production code |

### 4.1 Must never differ

Outcome class, errno, which thread unblocks, message bytes (compared by digest), receive-id validity transitions, per-thread event order, send→receive→reply edges, event counts, effective priority at receive, pulse code and value.

A difference in any of these is a model defect, not an allowed divergence.

---

## 5. Unverified assumptions

Carried forward from the design phase. Each is either resolved by a scheduled experiment or documented as a limitation.

| Assumption | Status | If wrong |
|---|---|---|
| Target headers declare the IPC surface as plain `extern` with no macros or inlines | Unverified against the 8.0 headers; verified only on older headers | Interposition needs the documented fallback path (`--wrap` or source-compatible shim), which is a scope increase |
| The default dispatch context message size is 2,048 bytes | Unverified | Affects one error-code reproduction in the conditional resource-manager tier |
| Receive-time priority assignment across multiple channels | Unverified | Decided by a two-channel conformance test; the model makes it a one-line change |
| Pulses are never coalesced | **Resolved 2026-11-14.** QNX 8.0 does compress identical consecutive pulses at one priority; because compression preserves delivery order and count, the observable sequence is unchanged and the model is equivalent (IMP-16) | Nothing observable; recorded as divergence #6 |
| A public hypervisor library surface exists | Out of scope | None |

---

## 6. Performance envelope

| Quantity | Value | Caveat |
|---|---|---|
| Non-blocking kernel call | 60–150 ns | Copy ≤ 256 B; measured, not estimated |
| Context switch, thread backend | 1.5–4 µs | The default backend; this is the number quoted in claims |
| Context switch, fiber backend | 60–300 ns | Opt-in, requires a clean thread-local audit |
| Throughput | ≥ 15 seeds/s/worker, 4-thread SUT | Thread backend, single core |
| 20,000 seeds on 16 cores | ≤ 3 minutes wall | Includes startup, merge, and a bounded number of shrinks |
| Memory per worker | ≤ 70 MB RSS | Arena is reserved but lazily committed |

These are engineering targets with pass/fail gates. Published figures name the hardware, the commit, and the backend.

**Explicit non-claim:** none of these numbers describe target performance. Permute is not a timing-analysis tool, and virtual time must never be used for worst-case execution time analysis.

---

## 7. Fidelity claim discipline

The claim is scoped, and the scope is part of the claim:

1. **12 mandatory conformance scenarios**, compared field by field against committed traces recorded from a real target. Target: at least 10 of 12 strict trace-equivalent; all 12 outcome-class equivalent.
2. **12 seeded model defects**, of which at least 10 must be caught by the scenario suite. A surviving mutant is a hole in the tests, not a reduced threshold.
3. **A negative control** — an intentionally racy program that must produce different traces across repeated target runs. If it does not, the comparison oracle cannot detect nondeterminism and the conformance job fails.
4. **Every divergence is listed** in §4 above and, once capture is performed, in `conformance/ALLOWED-DIFFERENCES.md` — each with a reason a reviewer can check.

What is not claimed: that the model is complete, that passing seeds imply correctness, that the simulator reproduces hardware timing, or that untested behaviour behaves like the target.

---

## 8. Failure behaviour when something is unsupported

Unsupported behaviour must be loud. The contract:

| Situation | Behaviour |
|---|---|
| Unsupported API called | Hard error with a stable code and a documentation link; never a silent success |
| Real clock or real signal requested | Build-time rejection via the symbol allowlist |
| Multi-process spawn attempted | Immediate `UNSUPPORTED_MULTIPROCESS` |
| Write to an undeclared path | `EROFS` plus a trace record |
| Budget exhausted | Outcome `BUDGET`, not a violation; reported separately |
| Empty ready queue and empty event heap | `OUTCOME_DEADLOCK`, not a hang |
| Replay against a mismatched build, kernel, or ABI | Refusal with a distinct exit code unless explicitly overridden |

A silent approximation is worse than a refusal: it produces confident, wrong results. If you find one, it is a bug.
