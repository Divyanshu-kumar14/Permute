<!-- SPDX-License-Identifier: Apache-2.0 -->

# Decisions

**Owner:** [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14)
**Status:** authoritative for this repository
**Last updated:** 2026-11-14
**Scope:** every architectural and scope decision that a reviewer, contributor, or future maintainer needs in order to understand *why* the code looks the way it does.
The product requirements document is a local working document and is not distributed. This file is the tracked, self-contained record. If this file and the code disagree, one of them is a bug: file an issue rather than editing history.

## How to use this file

- Decisions are numbered and **immutable**. Do not rewrite an accepted decision; add a new one that supersedes it and mark the old one `Superseded by NNNN`.
- Every decision has a status: `Accepted`, `Proposed` (needs an owner signature), `Superseded`, or `Rejected`.
- A change that contradicts an `Accepted` decision does not merge. A change that contradicts a `Proposed` decision needs the decision resolved first.
- Open decisions are tracked in [§4](#4-open-decisions). They are blockers, not background reading.

---

## 1. Architectural decisions

### ADR-001 — Interposition by strong-symbol shadowing from a static archive

**Status:** Accepted

**Context.** Three mechanisms can replace QNX IPC calls without editing the subject under test: linker `--wrap`, a `-include` macro shim, or a static archive whose strong definitions are linked ahead of libc.

**Decision.** `libpermute_guest.a` defines strong symbols for the interposed surface. Link order is `SUT objects … libpermute_guest.a … libc`. Because the linker is single-pass and libraries follow objects, our definition wins and libc's is never referenced.

**Consequences.**

- Positive: no per-call-site machinery; survives function pointers and vendor headers; replacing whole layers (`dispatch`, `resmgr`, `iofunc`) is natural.
- Negative: a partially shadowed layer group corrupts libc (our mutex with libc's condvar), so group atomicity must be a build gate.
- Negative: a vendor archive that already defines `MsgSend` produces a duplicate-symbol error, which must be reported clearly rather than suppressed.

**Rejected alternatives.** `-include` macro: changes the ABI being tested. `--wrap`: only rewrites undefined references in the SUT's own objects, so libc-internal calls are untouched.

**Fallbacks.** `-Wl,--wrap=<symbol>` for IPC-only deployments (documented as IPC-only). `LD_PRELOAD` for prebuilt binaries and the day-1 diagnostic probe.

---

### ADR-002 — Sticky message priority inheritance, implemented exactly as documented

**Status:** Accepted

**Context.** QNX message-driven priority inheritance differs from textbook PI: the server's priority does not revert when it replies. This is the single most visible fidelity error, and getting it wrong produces false bug reports.

**Decision.** Raise at send time for any receive-blocked thread on the target channel; assign at receive as the maximum over messages still pending on that channel, including the one dequeued; **no revert on reply**; only an explicit `SchedSet`, a higher-priority send, or a receive from a lower-priority sender change it; clamp to the process ceiling; honour `_NTO_CHF_FIXED_PRIORITY`.

Mutex PI (`PTHREAD_PRIO_INHERIT`, `PTHREAD_PRIO_PROTECT`) is separate and *does* revert on unlock.

Effective priority:

```
eff(t) = max(base_prio, sticky_prio, mutex_boost, max(prot_ceil of held PROTECT mutexes))
```

**Consequences.**

- Positive: conformance passes; bug reports are credible.
- Negative: a server can remain at a high priority indefinitely, which looks like a bug to anyone expecting textbook PI. Documented here and pinned by a conformance scenario.
- Open: multi-channel receive-time assignment semantics are unverified against the vendor documentation. A two-channel conformance test decides it, and the model makes it a one-line change.

---

### ADR-003 — Single-core execution model

**Status:** Accepted

**Decision.** Exactly one simulated CPU. Preemption points are the only scheduling freedom. No runmasks, no clusters, no migration.

**Consequences.**

- Positive: eliminates SMP nondeterminism as a category; makes the decision space coverable.
- Negative: SMP-only defects (migration-induced, per-cluster FIFO ordering) are out of reach. Documented in [`LIMITS.md`](LIMITS.md).

---

### ADR-004 — Counter-based PRNG keyed by (seed, domain, decision index)

**Status:** Accepted

**Context.** Shrinking requires forcing an individual decision without perturbing later ones. A stateful stream (xoshiro, PCG) shifts every later decision when one is forced, which makes shrinking non-monotone.

**Decision.** `splitmix64(seed ^ (domain << 40) ^ index)`, evaluated per decision, O(1) seekable. Seven independent domains: `D_SCHED`, `D_TIMER_TIE`, `D_RECV_ORDER`, `D_QUANTUM`, `D_STIMULUS`, `D_FAULT`, `D_CPUJITTER`.

**Consequences.**

- Positive: a decision can be forced or deleted and the run still works; adding a fault-injection call site cannot perturb the scheduling stream.
- Negative: `splitmix64` must be reimplemented identically in Python for the shrinker. A 10⁵-vector cross-language test guards against drift.

---

### ADR-005 — Process isolation via one shared object per process instance

**Status:** Accepted

**Context.** A QNX process has its own globals, statics, and static-initialisation order. Linking all services into one binary silently merges them, and conformance would then pass for the wrong reason.

**Decision.** Compile each process instance as its own shared object (`proc_srv__0.so`, `proc_cli__0.so`, …), `dlopen` with `RTLD_NOW | RTLD_LOCAL`, exporting a single `permute_main`. Spawn occurs at a scheduled `EV_SPAWN` event, so constructors run deterministically.

**Consequences.**

- Positive: genuine per-process `.data`/`.bss`; per-thread TLS is correct for free.
- Negative: N link steps for N instances.
- Rejected: `dlmopen(LM_ID_NEWLM)` (fragile per-namespace libc copy, capped namespace count, failures present as simulator nondeterminism); `fork()` per process (reintroduces real IPC and real time).

---

### ADR-006 — Thread backend with a baton by default; fibers opt-in

**Status:** Accepted

**Context.** `errno`, `__thread`, `pthread_self`, and signal masks are per-OS-thread in real QNX. A many-to-one green-thread model silently changes all of them.

**Decision.** OS thread per simulated thread with a baton handoff (~1.5–4 µs). Fiber backend (`ucontext`, ~60–300 ns) is opt-in and requires `symcheck --strict` to prove the SUT makes no real blocking syscall and touches no unvirtualised thread-local state.

**Consequences.**

- Positive: the failure a user sees is real.
- Negative: the headline throughput number is lower; published figures must quote the backend actually used.

---

### ADR-007 — Custom binary event log

**Status:** Accepted

**Decision.** Custom append-only binary `.pmt`: 64 KiB chunks, CRC32C per chunk, BLAKE3-64 hash chain, 20–24 bytes per record. NDJSON and Chrome Trace are **export formats only**.

**Consequences.**

- Positive: artifacts fit in an issue after shrinking; truncation yields a readable, marked-partial log rather than a crash.
- Negative: a codec to maintain and test; mitigated by golden files and a documented schema.

**Rejected alternatives.** Cap'n Proto (framing assumes a complete message tree; 8-byte alignment wastes ~40% on 20-byte records). JSON on disk (~9× larger, unstreamable at this volume).

---

### ADR-008 — Coverage-guided tape mutation as the default search policy

**Status:** Accepted

**Decision.** Default policy is novelty-guided mutation over a corpus of executed tapes, with feature maps over decision state and causal edges. `--explore random` remains available.

**Consequences.**

- Positive: sustained contention states are reachable; a coverage number becomes reportable.
- Negative: corpus management, a mutation operator set, and a merge command are real work, not a free win.
- Obligation: the novelty-over-random claim must be *measured* on the bug benchmark, not asserted.

---

### ADR-009 — Shrink inputs, never the event log

**Status:** Accepted

**Context.** Deleting an arbitrary event produces an invalid program: removing a reply leaves a send that can never complete.

**Decision.** Shrink the input space — stimuli, tape segments, fault plan. Use the causal graph only to decide what is worth attempting to delete. The shrinker has no operation that deletes an event.

**Consequences.**

- Positive: every candidate reduction is a valid execution, so every test is meaningful.
- Negative: the reported "minimal event count" is a consequence of the minimal input set, not an independently minimised quantity. Artifacts record `minimality: proven | budget_limited` and never claim more than was verified.

---

### ADR-010 — Host-only simulator; C capture agent for QNX

**Status:** Accepted

**Context.** The simulator does not need to run on QNX — deterministic simulation is a host activity — but evidence of genuine target use is required.

**Decision.** The simulator is host-only (Linux). A separate C11 capture agent (`libpermute_record.so` + `permute-capture`), built with `qcc` and run under QNX 8.0 in QEMU, records canonical traces from real IPC. Conformance builds the same scenario source for both targets.

**Consequences.**

- Positive: the runtime stack stays simple; target use is evidenced by a real build, a real boot, a real capture, and committed oracle traces.
- Negative: two artefacts to maintain; the capture agent needs a QNX SDP to build.

---

### ADR-011 — Apache-2.0, clean-room ABI headers

**Status:** Accepted. `LICENSE` is Apache-2.0 and `NOTICE` records the attribution as of 2026-11-14.

**Context.** QNX headers are copyrighted and cannot be redistributed. Host-only builds must compile QNX-style source without an SDP.

**Decision.** Ship hand-written prototypes for the interposed symbols, written from public documentation, never copied from vendor headers. Any other vendor header requires `--qnx-target` and a user-supplied SDP.

**Consequences.**

- Positive: the repository is distributable and licence-clean.
- Negative: prototype drift if the vendor changes a signature; mitigated by a signature test compiled against the real headers when an SDP is present.

---

## 2. Implementation decisions

These are decisions the architecture left open, resolved during planning.

| ID | Decision | Rationale | Status |
|---|---|---|---|
| IMP-01 | The shim ships as `libpermute_guest.a` (canonical) plus an optional `.so` build of the same objects for `LD_PRELOAD` mode | ADR-001 requires an archive; a `.so` keeps the documented fallback alive without changing the primary link model | Accepted |
| IMP-02 | Interposed-symbol ABI is generated from one X-macro (`abi/permute_iface.def`) carrying `(group, ret, name, params, flags)` | Prototypes, shim bodies, `allowlist.txt`, and the symbol audit all derive from one source; drift becomes impossible | Accepted |
| IMP-03 | `rcvid = (slot + 1) \| (generation << 16)`, always positive; generation bumps on reply and on cancellation | Detects stale and forged rcvids without a table scan; keeps `0` free for "this was a pulse" | Accepted |
| IMP-04 | Outcome taxonomy is one enum: `CLEAN, VIOLATION, CRASH, BUDGET, DEADLOCK_OUTCOME, LIVELOCK_SUSPECT, NONDETERMINISM, TIMEOUT_WALL, INTERNAL_ERROR` | Budget exhaustion must never be reported as a violation | Accepted |
| IMP-05 | Canonical trace comparison uses a topological linear extension with a total tie-break on `(normalised_src, thread_local_index, kind, obj)` | Reordering independent events must not create a false difference; virtual time and addresses must never be the tie-break | Accepted |
| IMP-06 | Shrinker pipeline order: causal pre-slice → stimulus ddmin → time-window minimisation → tape elimination → fault materialisation | Deterministic, monotone, and every candidate is a valid execution (ADR-009) | Accepted |
| IMP-07 | The deterministic allocator covers guest allocations; `free`/`realloc` dispatch by address range and forward non-arena pointers to libc | Guests legitimately receive libc-allocated pointers; a blanket interposition would crash | Accepted |
| IMP-08 | ASLR is disabled by re-executing the run process once with `personality(ADDR_NO_RANDOMIZE)`; the parent asserts identical `/proc/self/maps` layout | The personality flag has no effect on an already-running process | Accepted |
| IMP-09 | Guest stacks are 512 KiB, not 8 MB | Four 8 MB stacks would consume half the 70 MB RSS budget before the arena is touched; kernel-as-library recursion depth is small and asserted in debug | Accepted |
| IMP-10 | The hypercall page is mapped with `MAP_FIXED_NOREPLACE` from a candidate list; the chosen base is recorded in the run manifest and excluded from `run_digest` | Fixed addresses are not portable to hosts with a 39-bit user VA; determinism is per (build, host), and the manifest makes it visible | Accepted |
| IMP-11 | `show` is the canonical subcommand; `diff` may exist only as an alias | The requirement text uses both names | Proposed |
| IMP-12 | Guest thread IDs, `pthread_self()`, and process IDs are small deterministic ordinals allocated in spawn order | Addresses and host TIDs are not reproducible | Accepted |
| IMP-13 | All kernel state is global and single-owner; the kernel executes on the calling guest thread's stack | Single CPU (ADR-003); avoids a per-thread kernel context and a second scheduler | Accepted |
| IMP-14 | Pulse/event semantics are taken from the 8.0 reference, not from the PRD's simplified wording: a negative coid means `-chid` in the caller's own process; the pulse code is an 8-bit signed wire field; `MsgRegisterEvent` replaces the caller's event with an opaque handle and records the one server allowed to deliver it, so `MsgDeliverEvent`/`MsgVerifyEvent` are ownership checks rather than byte comparisons | Copying a byte compare would break every real client, because a client that registers an event sends a handle, not the original struct | Accepted |
| IMP-15 | `_NTO_CHF_DISCONNECT` is emitted on the client's **last** detach from a channel (including kernel-driven detach on process death), not on channel destruction | The 8.0 reference defines the flag in terms of client detach; the earlier reading was wrong | Accepted |
| IMP-16 | Pulse coalescing is **documented but not modelled** | QNX 8.0 compresses identical consecutive pulses at one priority into a 16-bit count. Because compression requires the most recently queued pulse at that priority to be identical, delivery order and the sequence a receiver observes are unchanged; a per-pulse queue is observationally equivalent and simpler to trace | Accepted |
| IMP-17 | The ready queue owns its links (`next`/`prev`/`prio`/`on`, indexed by slot); the thread struct does not participate in a list | Removal from the middle stays O(1), a queued slot is always a valid array index, and a thread cannot be half-linked | Accepted |
| IMP-18 | `SchedYield` is implemented as "requeue self at the tail, then pick" rather than a comparison against the bucket head | It yields to equal priority and can never yield to a lower one by construction, which is exactly FR-3.19, with one code path instead of a special case | Accepted |
| IMP-19 | Ready-queue membership is maintained by thread create/destroy and by priority changes; IPC blocking transitions are **not** wired to it yet | There is no executor until the run loop exists. Wiring six IPC transitions now would be untestable and would have to be rewritten; the boundary is stated in `ARCHITECTURE.md` §6 and in the P1-04 report | Accepted |
| IMP-20 | Message-driven inheritance follows PRD 9.1's `eff_prio = max(base, sticky, …)` even where the vendor text implies the inherited value can sit below the thread's own priority | The PRD is normative and its formula is explicit. The inherited value is still assigned and observable; only the effective priority is floored. Recorded as divergence #11 in `LIMITS.md` | Accepted |
| IMP-21 | Mutex inheritance is computed from the mutex table (`recompute_boost` scans INHERIT mutexes the thread owns) rather than maintained as a per-thread donor list | One source of truth, so a boost cannot drift from the waiters that justify it. A scan of 128 mutexes on unlock is cheap and deterministic | Accepted |
| IMP-22 | `PTHREAD_PRIO_PROTECT` recomputes `prot_ceil` from the held mutexes on every lock/unlock, and a lock attempt above the ceiling returns `EINVAL` | Matches "the mutex locking will fail" in the vendor text; a silent cap would be a wrong-priority run reported as clean | Accepted |
| IMP-23 | Block and wake are a single pair that changes thread state and ready-queue membership together; this supersedes the P1-04 boundary (IMP-19) | A timed-out thread that is not in the ready queue cannot be picked, which makes the timeout untestable end-to-end. One pair, used by every blocking entry point, removes the class of "queued while blocked" bugs | Accepted |
| IMP-24 | Timer heap entries are invalidated lazily by sequence number, and stale entries are purged before the clock moves | Removing from the middle of a heap needs an index; a sequence check is O(1). But a stale entry must not advance the clock, or replacing a timer is observable as a time jump - caught by a test | Accepted |
| IMP-25 | BLAKE3 is implemented portably in C (writer) and Python (reader) rather than pulled in as a dependency, and both are checked against the official vectors | The format's integrity claim must be verifiable by the reader, not trusted from the writer. Python has no stdlib BLAKE3. A 1025-byte vector exercises the two-chunk tree, which a single-block test would miss | Accepted |
| IMP-26 | Chunk hashing is `BLAKE3-64(prev_hash ‖ chunk minus CRC)`; the CRC is checked first and is not part of the hash input | The chain must cover the chunk header (which carries `prev_hash`) and all records; including the CRC would be redundant, and checking it first gives a cheaper, clearer corruption signal | Accepted |
| IMP-27 | The footer's reserved word carries the kernel model version | Replay must distinguish a kernel mismatch (exit 4) from a digest difference (exit 11). Without the version in the file, every kernel change would present as a digest mismatch and the exit-code contract in FR-6.6 would be unimplementable | Accepted |
| IMP-28 | Replay compares the manifest against values the caller supplies, never against the artifact's own header | A file that claims a different build id must not be able to authorize itself. The artifact's fields are data to check, not authority | Accepted |
| IMP-29 | `tools/replay_fixture` supplies "replay from seed" until the run loop exists | The mechanism under test is seed → decisions → trace → digest. The fixture exercises it through the real kernel without pretending to be the scenario executor; the run loop replaces it at P1-12 | Accepted |
| IMP-30 | Guest isolation uses one `.so` per instance with `dlopen(RTLD_NOW \| RTLD_LOCAL)`, **not** `dlmopen(LM_ID_NEWLM)` | ADR-005 rejected namespaces for stated reasons (fragile libc copy, capped count, failures that look like nondeterminism). Separate objects give real per-instance `.data`/`.bss` and correct TLS without them | Accepted |
| IMP-31 | The guest version script is generated from `abi/permute_iface.def`, checked for drift at build time, and `nm -D` is asserted on the output | IMP-02 makes the definition file the single source of truth; a hand-maintained export list would drift silently. `nm` proves the interposed symbols are defined in the object rather than imported from libc | Accepted |
| IMP-32 | The allocator reserves 2 GiB at a fixed address with a non-fixed fallback, and records the base actually used | A fixed address maximises reproducibility, but failing to start when it is occupied would make the tool unusable on some hosts. Offsets are base-independent, so digests stay comparable; the manifest records what was used | Accepted |
| IMP-33 | Free lists are ordered by offset with O(1) head/tail/LIFO paths and an O(n) middle scan, rather than an address-ordered tree | The requirement is that allocation returns the lowest free address, which the list guarantees. The common free patterns are O(1). A tree would change no observable behaviour; if a workload ever makes the scan hurt, it is a container swap behind the same contract | Accepted |
| IMP-34 | Requests above 2 MiB return `ENOMEM` instead of being routed to libc | Routing the largest allocations to libc would reintroduce exactly the allocator dependence the arena exists to remove. A loud refusal is honest; the limit is documented | Accepted |
| IMP-35 | Guest objects are linked with `-Bsymbolic-functions` | A dlopen'd object's references are resolved in the global scope first, so libc's clock/identity/rand symbols preempted the shim's own definitions. The guest then read the host clock silently. Found by a probe that hung for 54 years in `nanosleep`; binding intra-object calls is the fix | Accepted |
| IMP-36 | The determinism gate allowlists routing shims by path, not by exception comments | A path allowlist is auditable in one place; an in-source suppression comment can be added anywhere by anyone. Comments are stripped before scanning so documentation does not trip the gate | Accepted |
| IMP-37 | `tests/golden/rng_vectors.tsv` is committed, with `tools/gen_rng_vectors.py` to regenerate it | Both C and Python check the same fixed artifact instead of each other's live output, so a shared bug cannot hide. A diff in the file is a compatibility break and is reviewed as one | Accepted |
| IMP-38 | `rand`/`random` expand a per-process origin from the kernel, and share one stream | FR-4.11. libc's generator is seeded from the host and unspecified across versions; a guest using it would produce a different program on every machine | Accepted |
| IMP-39 | Crash isolation is one child process per seed, never a recovered fault in-process | A SUT SIGSEGV cannot be contained reliably in the same address space; a child also gives a clean wall-clock deadline via `SIGKILL`. The worker only ever sees a wait status | Accepted |
| IMP-40 | Batches are claimed with `O_EXCL` lease files, and staleness is "the recorded pid is gone" | The filesystem provides atomicity without a lock server. A dead pid is the only reliable liveness signal across workers, and `--steal` makes reclamation explicit rather than automatic | Accepted |
| IMP-41 | `CRASH` and `VIOLATION` set exit 1; `BUDGET` does not | FR-5.6 says a budget hit is an outcome, not a failure. A crash is a finding, and the overall run must say so while still completing every seed | Accepted |
| IMP-42 | The project configuration is a closed schema: unknown keys are rejected | A silently ignored typo in a budget or scheduler changes results without changing any output. Rejection with a stable code costs one line and prevents a class of unreproducible reports | Accepted |
| IMP-43 | Exit codes are owned by the CLI's top level: config 2, build 3, replay refusal 4, nondeterminism 5, deadline 6, missing harness 7, digest 11, interrupt 130 | FR-9.2 is about what a caller sees. A single mapping is testable, and the `internal-exit` harness command asserts the codes whose real trigger is still staged so the matrix cannot rot | Accepted |
| IMP-44 | `permute init` generates `symbol-audit.txt` and `header-gaps.md` from the definition file and the real archive | A scaffold that claims coverage it cannot show is worse than no scaffold. Both files are regenerated by the tools, so they reflect the checkout rather than a stale template | Accepted |
| IMP-45 | `abi/permute_qnx.h` defines `struct sigevent`, `struct _pulse`, the mask bits, channel flags, and pulse codes, and does **not** include `<signal.h>` | Scenario sources need QNX names to compile unchanged on both targets (FR-2.1). glibc defines a different `sigevent` in `signal.h`, so the compat header must precede it and signal-API scenarios wait for signal shims | Accepted |
| IMP-46 | Scenario coverage (`every primitive`, `all 11 mask bits`) is checked statically over the source | The executor does not exist yet, so a compile is the strongest end-to-end claim available. The checks are exact and fail the build, and they move to runtime assertions when the executor lands | Accepted |
| IMP-47 | Coverage is measured per source line, unioned across the object files that instrumented it, and grouped into kernel / shim / log / oracle | CMake names objects `foo.c.o`, so `gcov -o dir src` cannot find them; passing the `.gcno` directly works. A group with no sources reports `n/a` rather than a zero that would be indistinguishable from dead code | Accepted |
| IMP-48 | The musl job runs the unit suite, skips the glibc-internal allocator probe and the scenario set | `__libc_malloc` is glibc-only, and s07 needs pthread priority-ceiling attributes musl does not implement. NFR-11 asks the portability jobs for the unit suite, which is what runs | Accepted |
| IMP-49 | CI is a set of make targets, and the workflows only call them | A developer runs exactly what CI runs. `make ci-check` parses both workflows with PyYAML and asserts the structure, so a broken `runs-on` fails locally instead of on the runner | Accepted |
| IMP-50 | A cooperative yield requeues self and hands control to the scheduler; the bridge does not call `permute_sched_yield` | `permute_sched_yield` selects a thread inside the caller's host thread, so the baton never moves and the scheduler sees a running thread it cannot wake. `permute_thread_yield` plus a park is the only correct form | Accepted |
| IMP-51 | Guest objects export `permute_hypercall_bind`, and the loader calls it per object | Each `.so` has its own `g_page`; a guest that never binds sees a null table and every call fails with `ENOSYS`. Binding in the loader is explicit and testable | Accepted |
| IMP-52 | `SchedGet` reports the effective priority in `sched_priority` | The host `struct sched_param` has no `sched_curpriority`. The simulator maps the effective value into the field the host struct has, and the mapping is documented so a conformance comparison can account for it | Accepted |
| IMP-53 | A fault site draws one `D_FAULT` value per armed rule per invocation; an unarmed site draws nothing | Instrumentation must be free until a rule is armed, or adding a call site would shift every later fault decision. The scheduling stream is a different domain, so it cannot move at all (FR-4.3) | Accepted |
| IMP-54 | `DROP` at send blocks the sender in `SEND` with no message | A lost message is not an error the sender can see; it is a deadlock unless the sender armed a timeout. Modelling it as a returned error would hide the bug class the fault exists to reproduce | Accepted |
| IMP-55 | `DUPLICATE` is modelled at send, reply, and pulse; `REORDER` is a bounded delay; duplicate at receive is not modelled | Duplicating a received message requires a second kernel-owned copy and a second rcvid, which changes the reply contract. The actions that map cleanly are implemented; the others are named rather than approximated | Accepted |
| IMP-56 | Trace emission is shared by the executor and the bridge through one sequence counter | The writer rejects a decreasing seq. Two independent counters would collide the first time the executor emitted a record between two bridge records. All emission happens on the baton holder, so no lock is needed | Accepted |
| IMP-57 | `BLOCK` carries its peer in a 4-byte blob, not `dst` | The record schema for BLOCK has no `dst` field; adding one changes a kind's wire layout and invalidates every committed trace. A blob is part of the format already and carries the same edge | Accepted |
| IMP-58 | Deadlock requires every live thread blocked and no armed timeout, not just an SCC | A cycle with a pending timer is not necessarily permanent. The SCC alone would fire on a transient wait; the armed-timer check is what makes the finding a deadlock. When no cycle exists the finding says so rather than inventing an edge | Accepted |
| IMP-59 | Starvation counts dispatch decisions, not records | Under FIFO a running thread is not preempted, so a ready equal-priority thread waiting while the other runs is correct scheduling. Counting every event would call that starvation. `max_ready_gap_events` is the number of times the scheduler chose someone else | Accepted |
| IMP-60 | `priority_inversion` reports `no_inheritance_configured` as information, `pi_not_applied` at the configured severity | FR-10.4 requires the distinction. Inheriting is opt-in, so an inversion without it is an expected consequence of the scenario; only a configured inheritance that did not raise the owner is a kernel defect | Accepted |
| IMP-61 | `queue_depth.channel` takes the coid number or `*`, not a channel name | QNX channels have no names, and the trace records what exists. Accepting a name would silently never match, which is the failure mode this project treats as a bug. Names can return with a name-to-coid manifest | Accepted |
| IMP-62 | The shrinker's budget is spent before an execution, not after | 1,500 must mean at most 1,500. Checking afterwards would allow the bound to be exceeded by one; checking before makes the bound exact. A single execution that overruns the wall bound ends the pipeline immediately after it, which is the closest a non-preemptible callback permits | Accepted |
| IMP-63 | The causal pre-slice keeps inputs with no recorded effects | A rule that never fired or a stimulus with no trace record may still be necessary; only evidence of irrelevance removes an input. This keeps the slice sound and costs one verification execution | Accepted |
| IMP-64 | The shrink result excludes wall-clock time from its serialised form | FR-6.10 requires byte-identical output. `executions` and the reduced input are deterministic; elapsed milliseconds are not. Timing stays a display field and never enters the artifact | Accepted |
| IMP-65 | `permute shrink` refuses an artifact that no longer reproduces (exit 4) | A stale artifact must not be reduced to an input set that does not fail. The refusal is the difference between shrinking evidence and inventing it | Accepted |
| IMP-66 | Real-run shrinking currently reduces the fault plan only | The runtime consumes the seed and fault plan; stimuli, the window and a decision tape are recorded but not applied. Their stages reduce them to empty for a real artifact, which is correct for this runner, and the algorithms are exercised by input-tape fixtures (LIMITS #50) | Accepted |
| IMP-67 | A benchmark finding is a guest exit code of 1, not an oracle evaluation | The worker already maps exit 1 to VIOLATION. Requiring the benchmark to decode 60,000 traces would make the gate slow for no additional evidence; the fixtures assert their own postcondition, and the oracle suite covers predicates separately | Accepted |
| IMP-68 | The fixed baseline counts BUDGET and INTERNAL as false positives | A clean build must not exhaust a budget or fail the harness on a ten-instruction scenario. Treating only crashes and violations as findings would let a broken fixed fixture pass | Accepted |
| IMP-69 | Bug fixtures choose their race with a seeded `rand()` branch | The scheduler exposes little entropy for same-priority threads, so a fixture that is a genuine race cannot be made seed-dependent through priorities alone. The seeded branch is deterministic, reproducible and honest: the failing range is a property of the fixture, recorded as such in LIMITS #55 | Accepted |
| IMP-70 | Canonical object labels are scoped to the creating thread; threads are labelled by first appearance | A global first-appearance map renames two channels when their creation events are reordered. Scoping makes independent creation order irrelevant. Threads still rely on first appearance, recorded as LIMITS #61 | Accepted |
| IMP-71 | Trace equivalence checks both the canonical linear extension and the causal edge set | The extension alone can hide a missing edge when the content sort happens to produce the same order. Comparing the edge multiset makes the verdict exact for the normalised events | Accepted |
| IMP-72 | Committed conformance references are simulator output, labelled as such | The SDP is external input. Committing generated references makes the pipeline live and catches regressions, but they are not target evidence. Every report states the oracle source; the runner prefers `oracle/qnx/` when a capture exists | Accepted |
| IMP-73 | Conformance skips C11 and C13 explicitly and still exits 0 when the executed set passes | C11 needs process death (multi-process simulator) and C13 needs the mutex shim. A skip is named with its reason and excluded from the executed count; it is never reported as a pass | Accepted |
| IMP-74 | Mutants are compile-time defines, not runtime injection | A define cannot be accidentally enabled in a release build, adds no code path to the normal binary, and lets the mutant build be a normal CMake build with one extra option. `PERMUTE_MUTANT` is empty by default | Accepted |
| IMP-75 | A unit-test failure counts as a kill | The PRD's wording says "scenario", but three mutants are not observable through the cooperative guest: RR preemption needs a CPU-bound thread, the timeout latch is kernel state, and mutexes are not interposed. Killing them through the targeted unit suite is stronger than pretending a scenario sees them (LIMITS #66) | Accepted |
| IMP-76 | The mutation suite is the conformance set plus four focused scenarios | Conformance already covers ordering, pulses, PI, timeouts and teardown. s02/s03/s06/telemetry add the PI chain, mask bits, unblock contract and event delivery; the M07 fixture adds two channels. A mutation that none of them changes is a suite gap, not a pass | Accepted |
| IMP-77 | Exploration mutates the input tape (seed, faults, stimuli), not a persisted decision tape | The kernel exposes no decision-override API, so a tape of decisions cannot be replayed yet. The six FR-5.5 operators act on the inputs that do exist. The boundary is the shrinker's: inputs are valid to change, events are not (LIMITS #69) | Accepted |
| IMP-78 | A shard recomputes the whole exploration chain and records only its slice | Skipping earlier iterations would change the corpus and therefore the later mutations. Recomputing makes `--shard 2/4` byte-identical to shard 2 of a full run at the cost of redundant work (LIMITS #70) | Accepted |
| IMP-79 | The novelty benchmark reports its measured delta, not a threshold | On B1-B6 novelty found 74 non-clean outcomes against random's 70 and both found 6/6; time-to-first was 14 versus 13. The set is small and not held out, so the honest claim is "novelty covers more features and found no fewer bugs", not "novelty is faster" (LIMITS #71) | Accepted |
| IMP-80 | `--verify` samples the traces that exist, not the configured seed range | A seed that crashed may have no complete trace; sampling it would compare a digest that was never produced. The present traces are the only ones whose digest the run can claim, and the first seed is always included | Accepted |
| IMP-81 | A verification mismatch exits 5 even if the run found a violation | Nondeterminism invalidates the evidence for the violation. Exit 1 would imply the finding is reproducible when the harness just proved it is not | Accepted |
| IMP-82 | The planted nondeterminism changes the build id, not the scenario | The digest covers the build id, so one byte is enough to exercise the check without a racy SUT. The hook is environment-gated and test-only, like the crash and spin hooks | Accepted |
| IMP-83 | Per-seed report rows come from the trace list plus findings, not from a per-seed status file | The worker already records the non-clean outcomes; the traces name every seed that ran. Combining them avoids a second status channel and keeps a shard report to its own seeds |
| IMP-84 | JUnit is validated structurally, SARIF against the OASIS schema | No JUnit XSD validator is installed, and installing one at test time needs network. The structural check enforces the same invariants for the emitted subset; SARIF is small enough to vendor, so it gets the real schema |
| IMP-85 | The HTML report has no timestamp and no external reference | A timestamp makes the file differ between runs for the same evidence. The report is a snapshot of the run; the JSON remains the machine-readable source |
| IMP-86 | The executor baton is a spin-then-yield pair of atomics, not two semaphores | Semaphores cost four syscalls per cooperative switch and measured 5.3 µs, above the 4 µs NFR. Atomics with `sched_yield` every 256 spins measure 0.9–1.1 µs. The scheduler occupies a core while the guest runs; that is the price of the bound and is stated in LIMITS #79 |
| IMP-87 | The G1 scorecard runs the real checks rather than reading prior results | A scorecard that trusts a previous green is not a gate. `make gate1` re-runs determinism, conformance, the benchmark, mutation, exploration, verification, reports, the switch benchmark and ASan, and writes the measured values |


---

## 3. Documented conflicts and how they are handled

| ID | Conflict | Resolution |
|---|---|---|
| C-01 | `LICENSE` was MIT; the licence decision is Apache-2.0 | **Resolved 2026-11-14.** `LICENSE` replaced with the canonical Apache-2.0 text, `NOTICE` added, SPDX identifiers applied. No MIT-licensed code was ever released |
| C-02 | Python 3.12 is required; the generic `bookworm-slim` image ships 3.11 | Use a `python:3.12-slim-bookworm` base; glibc match is preserved |
| C-03 | The shim is described as a `.so` in one place and a static archive in another | IMP-01: archive is canonical, `.so` is a preload-only build |
| C-04 | A 2 GiB arena is required alongside a 70 MB RSS ceiling | Arena is reserved `PROT_NONE` and committed lazily in 2 MiB windows |
| C-05 | Resource-manager support is specified as Tier 2 "not planned" while one of its requirements is marked MUST | Conditional: a timeboxed kill-criterion spike decides; if dropped, the deviation is recorded in `LIMITS.md` |
| C-06 | Scenario count is 16 in the table but 12 in the gate | 16 implemented, 12 mandatory and compared; the gate denominator is 12 |
| C-07 | The log format describes "20 record kinds" but the kind list enumerates 22 | One byte has room for the enumeration, which is authoritative; the count in prose is corrected to 22. The schema, not the prose, defines the valid set |
| C-08 | The PRD describes `MsgVerifyEvent` as a byte comparison, but QNX 8.0 replaces a registered event with an opaque handle | Implement the vendor mechanism (IMP-14). Verification compares the meaningful fields of the normalised 32-byte event and checks handle ownership; the padding words of `struct sigevent` are unspecified, so a literal byte compare would reject valid events |
| C-09 | FR-3.23 says `TimerTimeout(ntime == NULL)` disarms; QNX 8.0 says `ntime == NULL` means "no timer, immediate `ETIMEDOUT` on the next blocking state" (the polling idiom), and `flags == 0` disarms | Implement the PRD trigger and the vendor's `flags == 0`. Real QNX code that polls with `ntime == NULL` must be translated by the shim to a zero-length timeout. Recorded in `LIMITS.md` divergence #13 |
| C-10 | FR-3.21/3.22 require an armed timeout to survive a non-matching blocking call; QNX 8.0 says the next kernel call disarms it when it exits, whatever state it blocked in | Follow the PRD: the intent survives until a covered state is entered, is consumed by a call that completes in a covered state, or is replaced/disarmed. The models agree in the documented usage pattern (arm immediately before the call being timed). `LIMITS.md` divergence #14 |
| C-11 | The trace format says "Header 64 B" but lists fields totalling 72 bytes | The fields are authoritative: the header is 72 bytes (`ARCHITECTURE.md` §8, `kernel/trace.h`). Dropping a field to fit the prose would lose `created_unix_ns` or half the build id |

---

## 4. Open decisions

These need a signature from the named owner. They are ordered by how much they block, and each one blocks something concrete — they are not a wish list.

Identifiers are prefixed `OD-` so they never collide with the local planning register (which uses `D-`). Once a decision is signed it is copied into §1 or §3 and marked resolved here.

| ID | Decision | Options | Recommendation | Blocks | Owner |
|---|---|---|---|---|---|
| OD-02 | Staffing and scope | one builder / two builders / disclosed prior art | Two builders. The work breakdown is ~256 builder-hours against ~260 available; one builder cannot reach the full conformance claim | start of implementation | Owner |
| OD-03 | Conformance rescope if the schedule slips | keep 12 / drop to 8 | If dropping, the gate rescales to ≥6/8 strict + 8/8 outcome-class **by prior signature**, never after the fact | gate review | Owner |
| OD-04 | Resource-manager work | drop / timeboxed spike | Timeboxed spike with an explicit abandon condition; result recorded in `LIMITS.md` either way | resource-manager requirements | Owner |
| OD-05 | Prior-work disclosure | none / disclose | Disclose anything reused in `NOTICE` and `README` | submission | Owner |
| OD-06 | CLI surface | `show` / `diff` | `show`, per IMP-11 | CLI freeze | Maintainer |
| OD-07 | Pulse coalescing | model / leave unmodelled | **Resolved 2026-11-14.** QNX 8.0 documents compression of identical consecutive pulses at one priority; it cannot reorder delivery, so the model leaves it unmodelled (IMP-16) | fidelity documentation | Maintainer |

---

## 5. Rejected scope

Recorded so the question is not relitigated: unmodified QNX binary support; full kernel/driver/network emulation; deterministic SMP; a GUI or live dashboard; a plugin system or scenario DSL; Windows and macOS parity; QNX 7.1/6.5; IDE integration; a hosted cloud runner; LLM-based triage; formal verification or a TLA+ specification; real-time performance claims; full POSIX resource-manager coverage; distributed multi-machine simulation; record/replay on real hardware.

Reasons: none of these increase the evidence that the simulator matches QNX semantics, and each costs days that the fidelity work needs.

Target versions 7.1 and 6.5 are also rejected, and that decision is closed — the semantics differ in ways this model cannot express, and a version matrix would multiply the conformance surface without improving the fidelity claim.
