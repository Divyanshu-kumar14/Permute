<!-- SPDX-License-Identifier: Apache-2.0 -->

# Decisions

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
