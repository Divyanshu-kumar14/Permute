<!-- SPDX-License-Identifier: Apache-2.0 -->

# Day-1 spike report — interposition and handoff cost

**Date:** 2026-11-14
**Commit:** `e67b2a0` (spike landed after this report)
**Purpose:** settle the two assumptions the whole design rests on, before any kernel work.
**Result:** **both pass.** Shadowing works on the host; handoff cost is inside budget on both the spinning and the parking path.

---

## 1. Environment

| Item | Value |
|---|---|
| CPU | 13th Gen Intel Core i5-13450HX (16 logical CPUs) |
| OS | Linux, glibc, x86_64 |
| Compiler | GCC 16.2.1 |
| CMake / Ninja | 4.5.0 / 1.13.2 |
| Build type | `RelWithDebInfo` (`-O2 -g`) |

Numbers below are from this machine only. They are a feasibility check, not a published benchmark.

---

## 2. What was actually tested

A positive test can pass by accident, so the spike is built as four independent proofs:

1. **Direct binding** — calls to five IPC symbols reach the archive.
2. **Indirect binding** — the same call through a function pointer reaches it too (a pointer cannot be resolved by a header macro).
3. **Precedence over libc** — a symbol libc *also* defines (`write`) binds to our archive. `MsgSend` has no libc competitor on Linux, so testing only `MsgSend` would prove nothing about link order.
4. **Negative control** — the same program fails to link *without* the archive, on an undefined `MsgSend`.

Plus one informational probe for the open question Q1.

---

## 3. Shadowing results

```
PASS  direct ChannelCreate reaches the shim
PASS  direct ConnectAttach reaches the shim
PASS  direct MsgSend reaches the shim
PASS  MsgSend through a function pointer reaches the shim
PASS  direct MsgReceive reaches the shim
PASS  direct MsgReply reaches the shim
PASS  no kernel installed -> -1/ENOSYS, no silent success
PASS  direct write() binds to the probe archive, not libc
INFO  printf() reached the write probe: no (0 call(s))
```

**8 pass, 0 fail.**

### 3.1 Link-order evidence

The link line CMake produces (libc is appended by the compiler driver *after* the archives, which is the property ADR-001 depends on):

```
cc ... spike_shadow.c.o -o spike_shadow libpermute_guest.a libshadow_probe.a -lc
```

Archive contents:

```
T ChannelCreate   T ConnectAttach   T MsgSend   T MsgReceive   T MsgReply
```

Undefined IPC symbols remaining in the executable: **none**.

In the executable's dynamic symbol table, `write` is `GLOBAL` / `DEFAULT`, i.e. it interposes for the whole process, not just for calls compiled into our object.

Negative control:

```
PASS: link without libpermute_guest.a fails on an undefined MsgSend
```

---

## 4. Q1 host baseline (important, and easy to misread)

`printf()` did **not** reach the `write` interposer.

That is expected on glibc: stdio calls an internal alias (`__write`) rather than the public `write` through the PLT, so the call never leaves libc. It is not a bug in this spike, and it is **not** the answer for QNX.

What it does establish:

- Application-level interposition is reliable.
- **Library-internal callers cannot be assumed to route through the shim.** This is exactly the risk ADR-001 calls out, and exactly why the target-side probe (task P0-07) is mandatory and cannot be replaced by a host experiment.
- It also sets the expectation for `dispatch`/`resmgr`: if the target's library calls IPC internally, that layer must be replaced, not shadowed. The probe decides which.

**Decision impact:** none yet. Q1 remains open until read on real QNX. But the fallback reasoning is now backed by a measured host analogue rather than intuition.

---

## 5. Handoff cost

Two configurations, 20,000 handoffs each, 6 trials with trial 0 discarded as warmup. A "round trip" is one post + one wait, i.e. **two** context switches.

| Path | ns / round trip | ns / switch | Budget (≤4000 ns/switch) |
|---|---|---|---|
| Spin-then-park, 128 spins (default) | 300–353 | **150–176** | pass, ~23× headroom |
| Always park (`PERMUTE_SPINS=0`) | 4,022–4,438 | **2,011–2,219** | pass, ~1.8× headroom |

### 5.1 Interpretation

- The **parked** path matches the design assumption of 1.5–4 µs per switch. The budget in the plan is therefore calibrated correctly, and the fallback (fiber backend) is not needed for feasibility.
- The **spinning** path is an order of magnitude cheaper, but it buys that by burning a core while waiting. It is only valid when there is at least one idle core per runnable simulated thread — true for one worker per core, false under oversubscription.
- Consequence for throughput: at ~2.2 µs/switch and roughly one handoff per three events, a 50,000-event seed costs ~37 ms of handoff time, i.e. ~27 seeds/s/worker — inside the 15–25 seeds/s target once other costs are added. At ~0.16 µs/switch with spare cores, the same seed is ~6 ms.

Both paths must stay available: the scheduler cannot assume it owns a core.

---

## 6. What this does not prove

| Not proven | Why it matters | When it is resolved |
|---|---|---|
| Target-side interposition | The host has no QNX libc; the Q1 answer may differ | P0-07, with an SDP |
| Payload copy cost at send | Kernel currently forwards a pointer; FR-3.7 requires a copy | P1-02 |
| Determinism | No trace, no clock virtualisation, no ASLR control yet | P0-06 |
| Scheduler fidelity | No scheduler exists | P1-04 |
| Throughput end to end | Only the handoff primitive was measured | P3-06 |

Nothing here should be quoted as a fidelity or performance result for the product.

---

## 7. Verdict and next step

**Gate:** day-1 feasibility — **PASS** on the host half.

- ADR-001's mechanism works: strong symbols in an archive linked before libc win, directly and through pointers, and the negative control confirms the symbol came from us.
- NFR-4's handoff budget is achievable with the thread backend, which is the default and the correctness-critical one.
- Fallback A (source-compatible shim) is **not** needed on this evidence.

**Next:** P0-04 completion (extend the surface as tests appear), then P0-05 (hypercall page + vtable publication), then P0-06 (determinism bootstrap: `ADDR_NO_RANDOMIZE`, maps assertion, counter PRNG). The target probe (P0-07) runs in parallel as soon as the SDP is available, because Q1 is still the largest open technical risk.
