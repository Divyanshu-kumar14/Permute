<!-- SPDX-License-Identifier: Apache-2.0 -->

# FAQ

**Owner:** [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14)
**Status:** current for the P1 build.

---

### Is this a QNX emulator?

No. It does not execute QNX binaries, and it does not model hardware. It models
the IPC and scheduling semantics that message-passing services depend on —
channels, messages, replies, pulses, priority inheritance, timeouts — and runs
your code against that model.

### Does it run my existing binary?

No. Rebuild against the shim. The source is unchanged; only the link line
changes, because the interposed symbols come from a static archive linked ahead
of libc (ADR-001). The same source builds for QNX and for the simulator.

### Why not just use QEMU?

QEMU gives you one schedule per attempt, selected by a real kernel and real
timing. Reproducing a rare interleaving means reproducing a machine state you do
not control. Permute makes the schedule an input: every decision is
`rnd(seed, domain, index)`, so a run replays exactly and a different schedule is
one seed away. QEMU is still used — for the conformance oracle, where the point
is to observe the real target.

### Do I need a QNX licence?

Not for the simulator. A QNX SDP is needed only to build the capture agent and
the target side of conformance.

### Is it real-time?

No. Time is virtual and advances only when nothing is runnable. Latency and
jitter measured here are meaningless. This is not a timing-analysis tool and
must not be used for worst-case execution time.

### How deterministic is it?

A run is a pure function of `(seed, resolved config, SUT build)`. The test suite
runs 100 seeds × 10 separate processes and requires one digest per seed and at
least 95 distinct digests across seeds. There is no wall clock in the trace, no
pointer value in a hash, and no unordered container with observable iteration
order; the build gate fails if production code calls a real clock, real entropy,
or host identity.

### What is modelled, and what is not?

[`LIMITS.md`](LIMITS.md) is the authoritative list. In short: the IPC surface,
scheduler, priority inheritance, timeouts, and the trace format are modelled;
SMP, hardware timing, signals as asynchronous interrupts, and target-binary
execution are not. Anything unmodelled is supposed to fail loudly rather than
approximate.

### Why does an invariant say "not evaluated"?

The built-in seeded workload writes IPC records but not thread records, so an
oracle that needs `THREAD_START`, `BLOCK`, or `SCHED_PICK` cannot say anything
about it. Rather than pass silently, the run reports the invariant as not
evaluated. `permute run --guest PATH` executes a compiled scenario and writes
the records those oracles need.

### How do I execute my scenario source?

```sh
permute build
permute run --guest .permute/build/proc_smoke__0.so
```

The executor loads the object, binds it to the kernel table, and runs
`permute_main` as the first simulated thread. `permute run` without `--guest`
still executes the built-in seeded workload, which is what the worker's
isolation and budget tests use. Mutex and signal shims are not modelled, so a
scenario that depends on them compiles but is skipped at execution.

### Can I see what a run did?

Yes. Every run writes a `.pmt` trace: `permute show --seed N` summarises it,
`--format ndjson` exports records, and `--format chrome` produces a trace that
opens in Perfetto. `permute replay artifact` verifies the chunk chain, the
ordering, and the digest.

### What happens if the SUT crashes?

The worker runs one process per seed. A `SIGSEGV` becomes a `CRASH` finding, the
run continues, and the exit code says a finding was present. The worker itself
never executes SUT code.

### Is my platform supported?

Linux x86_64 with glibc is the primary target and runs on every change. musl and
aarch64 are nightly CI jobs; both run the unit suite. macOS and Windows are not
supported.

### How do I contribute?

The project is pre-release. Read [`ARCHITECTURE.md`](ARCHITECTURE.md) and
[`DECISIONS.md`](DECISIONS.md) first; a change that contradicts an accepted
decision does not merge without a new decision that supersedes it.
`make ci` must be green.

### What licence is it under?

Apache-2.0. See [`LICENSE`](../LICENSE) and [`NOTICE`](../NOTICE). No vendor
headers are shipped; the ABI declarations are hand-written from public
documentation (ADR-011).
