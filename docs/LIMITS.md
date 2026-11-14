<!-- SPDX-License-Identifier: Apache-2.0 -->

# Limits

**Owner:** [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14)
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
| 26 | `permute-run` executes the seeded scenario, not a scenario source yet | The executor and scenario loader are later tasks. The worker's isolation, leases, budgets, and reporting are independent of what the child runs, so they are tested now against the same seeded workload replay uses |
| 27 | No global wall deadline (`--total-wall`) | Only the per-seed budget is enforced. FR-5.6's 2 s wall is per seed; the run-level deadline belongs with the CLI's report loop |
| 28 | `--jobs` parallelises across worker processes, not inside one | Each worker is single-threaded over seeds. This matches FR-5.1's "N seeds across M workers" and keeps lease behaviour simple |
| 29 | `permute run` does not load the scaffolded guest `.so` yet | The runner executes the seeded scenario (#26), so `build` proves the guest compiles and `run` proves the pipeline works, but the two are not yet connected. Wiring the loader is the executor task |
| 30 | `tools/permute` is a launcher, not an installed entry point | Packaging and console-script installation are a distribution concern. The launcher sets `PERMUTE_HOME` and `PYTHONPATH`, so the same commands work from a checkout |
| 31 | Exit 10 has no user-facing trigger | "Violations differ" needs the oracle engine. The code is in the mapping and asserted through the `internal-exit` harness command so FR-9.2's matrix is complete and does not silently rot |
| 32 | Scenarios compile but are not executed end to end | `permute-run` still runs the built-in seeded scenario (#26), and `pthread_create`/mutexes resolve to libc until the executor and pthread shim exist. `make scenarios` therefore proves compilation and static coverage, not behaviour |
| 33 | Seven timeout mask bits are armed but their blocking calls are not modelled | `SIGSUSPEND`, `SIGWAITINFO`, `MUTEX`, `CONDVAR`, `JOIN`, `INTR`, `SEM` need signal, condvar, semaphore, and interrupt shims. The bits are exercised in the TimerTimeout call and named in the scenario; runtime coverage follows those shims |
| 34 | Oracle coverage is `n/a` | There are no oracle sources until P2. A zero would be indistinguishable from dead code, so the report names the gap instead |
| 35 | The musl job skips the interposed allocator probe and the scenario set | `__libc_malloc` is a glibc internal, and s07 uses pthread priority-ceiling attributes musl lacks. The allocator core is still tested through the prefixed object; the unit suite is the NFR-11 acceptance |
| 36 | aarch64 coverage is nightly and emulated | The nightly workflow runs the suite in an arm64 container under QEMU. It proves the code builds and the tests pass on the architecture, not performance |
| 37 | `s07_priority_ceiling` compiles but is not executed | glibc does not implement `PTHREAD_PRIO_PROTECT` (it returns `ENOTSUP`) and the mutex shim is still pending. The executor check skips it by name rather than faking a pass |
| 38 | `SchedGet` maps the effective priority into `sched_priority` | The host struct lacks `sched_curpriority`; a conformance comparison must account for the mapping (IMP-52) |
| 39 | `pthread_cancel` takes effect at the target's next shim boundary | True asynchronous cancellation would require stopping a host thread mid-instruction. The scenarios cancel blocked threads, where the boundary is immediate |
| 40 | Guest execution traces core events only | SEND, RECV, REPLY, ERROR, PULSE, EVENT_DELIVER and TIMER_ARM are emitted by the bridge; scheduler and block records land with the trace instrumentation task |
| 41 | Duplicate at receive is not modelled; reorder is a bounded delay | A duplicated received message needs a second copy and a second rcvid, which changes the reply contract. Reorder's observable effect is a reordered arrival, which a delay produces without claiming to reorder the queue |
| 42 | Fault injection applies on the guest bridge path only | The built-in seeded scenario does not go through the bridge, so faults are configured for guest execution (`--guest`). `permute run` rejects a non-empty `[faults] inject` without `--guest` rather than silently ignoring it; the worker's default path gains faults when the scenario loader becomes the default |
| 43 | `KILL` takes effect at the target's next shim boundary | Same constraint as `pthread_cancel`: stopping a host thread mid-instruction is not possible without asynchronous cancellation |
| 44 | The priority-inversion oracle has no real fixture yet | The mutex shim is pending, so mutex `BLOCK` records do not exist in a run. The predicate is exercised by record-stream fixtures, including both inheritance classifications; a real fixture lands with the mutex shim (`#37`) |
| 45 | `queue_depth` identifies a channel by coid | Channels have no names. `channel` is the number from the trace or `*`; a name is rejected rather than silently unmatched (IMP-61) |
| 46 | Thread-state oracles skip the built-in workload | Its trace has no `THREAD_START`/`BLOCK`/`SCHED_PICK` records, so deadlock, unbounded_block, priority_inversion, starvation and queue_depth are reported as "not evaluated" rather than passing silently. `--guest` supplies the records |
| 47 | No scenario produces a live reply cycle | The post-hoc oracle is fixture-tested and the live detector is unit-tested; `permute-run` prints `CYCLE=` for a live SCC, but no committed scenario constructs one yet |
| 48 | `lost_pulse` fires on the short-buffer drop only | A pulse queued at trace end may be delivered later; without a drop record it is not called lost. Pulse-queue overflow is not modelled yet |
| 49 | Oracle evaluation is one pass per invariant over the decoded trace | Correctness first: seven passes over a large trace cost more than a fused pass. Batching belongs with the throughput work (P3-06) |
| 50 | The runtime consumes the seed and fault plan, not stimuli, windows or a decision tape | Those inputs are recorded in the artifact and reduced by the shrinker, but changing them cannot change today's execution, so a real shrink drops them to empty. The stage algorithms are exercised by input-tape fixtures; the runtime gains the inputs before conformance |
| 51 | The shrinker has no pre-emption of a running candidate | A candidate that overruns the wall bound is stopped only after it returns. The per-execution timeout in the real runner bounds it to 30 s, and the pipeline stops immediately afterwards |
| 52 | Failure artifacts carry the trace path, not a compressed tape | There is no executed tape to compress yet (LIMITS #50). The artifact records the trace path and digest; moving the artifact without the trace loses the cone and the shrink becomes budget-limited rather than wrong |
| 53 | Shrinking reduces one artifact at a time | `permute shrink` takes one file. Batch shrinking and merging cases belong with the corpus work |
| 54 | Oracle ids in artifacts are the configured ids | A run with two same-kind invariants records the first error's id; the shrink predicate retains that rule and kind. Distinguishing two same-kind findings needs the full violation set, which is P2-09 report work |
| 55 | Bug fixtures choose their racy path with a seeded `rand()` branch | The scheduler is FIFO within a priority, so same-priority races have no seed-dependent interleaving. The branch models the race outcome and makes the expected range reproducible; it is a fixture property, not evidence that the scheduler explores interleavings |
| 56 | The B2 fixture lets the server reach `MsgReceive` before the client sends | A `TimerTimeout` mask of `_NTO_TIMEOUT_REPLY` armed while the client first blocks in SEND does not activate when the message is received and the thread becomes REPLY-blocked. The fixture avoids depending on that transition; whether QNX fires there needs the SDP. Recorded as a modelling gap |
| 57 | The bug benchmark runs 60,000 processes for the fixed baseline | One process per seed is the isolation contract. The full run is about 31 seconds on eight jobs, so it is a gate target (`make bench-bugs`), not part of `make ci`; CI runs the 100-seed quick form |
| 59 | Committed conformance references are simulator output | The QNX SDP is external input and no capture has been performed. The references guard against regressions and exercise the canonicaliser; they are not evidence of target fidelity. The runner labels the source in every report and prefers `oracle/qnx/` when present |
| 60 | C11 and C13 cannot execute in this simulator | C11 needs process death and the simulator runs one guest process; C13 needs the mutex shim. Both sources compile and are part of the set; they are skipped with a reason, never counted as passes |
| 61 | Conformance thread labels use first appearance | Two independent threads created in the opposite order on the target could swap labels even with isomorphic partial orders. Object labels are scoped to the creator to avoid the same problem for channels and coids; threads need a capture-side id map |
| 62 | The QNX build of the scenarios has not been attempted | The sources are written for both targets (`__QNXNTO__`), but `qcc` and the SDP are absent. FR-11.1's second build is unproven until the capture is run |
| 63 | C14 observes cooperative alternation, not CPU preemption | Guest code runs between shim calls without preemption, so a CPU-bound loop cannot be timesliced. The scenario alternates through `nanosleep`; the QNX run is the authoritative timeslice check |
| 65 | Mutants are compile-time only | `PERMUTE_MUTANT_Mxx` is set by the mutation runner's CMake build and is absent from every release build. There is no runtime switch and no mutant code in the normal binary |
| 66 | Three mutants are killed by unit tests, not scenarios | M06 (RR quantum) needs CPU-bound preemption, which the cooperative guest does not have; M09 (timeout latch) is kernel state not exposed by a scenario; M11 (mutex PI) needs the mutex shim. Their targeted tests are stronger evidence than a scenario that cannot observe them |
| 67 | `make mutation` is not part of `make ci` | Twelve CMake builds: about 25 s when warm, a few minutes cold. CI runs the conformance and bench quick forms; the full mutation score is a gate target |
| 69 | Exploration mutates inputs, not a decision tape | The six operators change the seed, fault rules and stimuli. The kernel has no decision-override API, so a recorded decision tape cannot be replayed; the novelty features still come from the trace, and the corpus is real |
| 70 | Every shard recomputes the full exploration chain | Correct shard equality needs the same corpus, and the corpus is sequential. `--shard i/n` runs all iterations and records its slice; the gate compares byte-identical entries, not runtime |
| 71 | The novelty-vs-random measurement is not a held-out benchmark | B1-B6 are the six fixtures from P2-04, not unseen defects. The measurement is 74 vs 70 findings, 6/6 found by both, first-finding sum 14 vs 13; it supports "no worse, more coverage", not a general speed claim |
| 73 | `--verify` skips seeds without a complete trace | A crashed or truncated run cannot produce a digest to compare; those seeds are already findings. The sample covers the evidence that exists, and the count is reported |
| 74 | `--verify` in a shard run checks only that shard's seeds | The trace directory holds the shard's runs. Verifying the whole range would require running seeds the shard did not claim; a full-run verification is the whole-run mode |
| 76 | JUnit validation is structural, not XSD-backed | No JUnit XSD validator is shipped or installed, and fetching one at test time would need network. `validate_junit` checks element nesting, required attributes and count agreement for the subset emitted; a consumer that needs the full XSD can validate the same file |
| 77 | The SARIF schema is vendored | CI must validate without network. `schemas/sarif-2.1.0.json` is the OASIS 2.1.0 schema used verbatim, with its source recorded in `schemas/README.md` |
| 79 | The scheduler spins while a guest thread runs | The baton is lock-free, so the scheduler occupies a host core for the duration of a guest's run between shim calls. `sched_yield` every 256 spins keeps a single-core host progressing. The measured switch is 0.9–1.1 µs; the busy core is the cost |
| 81 | FR-7.2 and FR-7.3 are dropped | The canonical "Bones of a Resource Manager" skeleton does not compile or link: the clean-room ABI has no `sys/dispatch.h`/`sys/iofunc.h` and the runtime has no `dispatch_*`, `resmgr_attach`, `iofunc_*` or pathname namespace. The kill criterion failed within its timebox; evidence in `docs/DECISIONS.md` §4.1 and `gate/p2-11-tier2-evaluation.md` |
| 82 | FR-7.5 is not implemented | It was conditional on Tier 2 (§18.2 X10, D-15). Tier 2 did not pass, so `dispatch_handler` non-preemption is not claimed. The PRD marks it M inside explicitly unplanned work; this is a recorded deviation, not a silent omission |
| 84 | Bench numbers are from one machine | The published results name the CPU (i5-13450HX, 16 cores), kernel and commit. They are evidence for this machine, not a guarantee on another; `make bench` must be re-run there |
| 85 | The kernel-call number is below the NFR-3 band | 5 ns measures the interposition path (indirect call + thread lookup), not a kernel entry or syscall. The 60–150 ns band assumes a real kernel call; being faster is not a failure, and the report says so |
| 86 | Seed throughput uses a synthetic four-thread fixture | It measures process setup, thread creation and the baton, not a real SUT's work. The 20,000-seed wall and per-worker rate are therefore an upper bound for this fixture, not a promise about every workload |
| 83 | `name_attach` is retained but not implemented | FR-7.4 is the only resource-manager-adjacent API in scope after the drop. It has no implementation today, and P3-09 is not armed. The simulator is one guest process, so a future name table would be process-global rather than system-wide |
| 80 | The G1 scorecard's conformance row uses simulator references | The row measures the harness and the scenario set, not QNX fidelity. The scorecard carries the caveat; the physical capture is still external input (LIMITS #59) |
| 78 | Report rows cover seeds that ran, not the configured range | A shard reports its slice; a seed with no trace and no finding was never executed. The counts in `report.json` remain the authoritative totals |
| 75 | The planted nondeterminism hook lives in the runner | It is environment-gated and test-only, alongside the crash and spin hooks. A release user who does not set the variable gets no hook; a user who does set it gets exactly the diagnostic the test expects |
| 72 | `LIVELOCK_SUSPECT` is a budget ratio, not proof of a spin | More than 2% budget outcomes can also come from expensive seeds. It is a signal to investigate, matching FR-5.8's wording; the spin fixture is the positive test, not the definition |
| 68 | The mutation set is the PRD's twelve, not arbitrary faults | The point is to test the suite against known semantic defects, not to maximise a mutation score. Each hook is a one-line semantic change with a stated expected detector |
| 64 | Six fixtures, four classes | B1/B2 are message-lifecycle bugs, B3 is scheduling, B4/B5 are queue/notification assumptions, B6 is resource lifetime. The PRD's four classes are represented; the mapping of B4 and B5 to the classes is documented in the baseline |

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
