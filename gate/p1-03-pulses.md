<!-- SPDX-License-Identifier: Apache-2.0 -->

# P1-03 report — pulses and secure events

**Date:** 2026-11-14
**Scope:** `MsgSendPulse`, `MsgReceivePulse`, `MsgRegisterEvent`, `MsgDeliverEvent`, `MsgVerifyEvent`, negative coid addressing, disconnect notification, and the teardown paths that were owed from P1-01/P1-02.
**Result:** **21,351 checks, 0 failures**, clean ASan/UBSan, full `make test` green.

---

## 1. What was built

| Path | Purpose |
|---|---|
| `kernel/pulse.c` | Pulse posting and delivery, the five entry points, disconnect notification |
| `kernel/ipc_internal.h` | The `msg.c` ↔ `pulse.c` seam: two functions, no public surface |
| `kernel/msg.c` | Rebuilt on `permute_coid_to_channel`; message delivery now skips pulse-only waiters |
| `kernel/objects.c` | Coid resolution, queue `pop_matching`, thread/channel/process teardown |
| `kernel/state.h` | ABI constants, 32-byte `permute_sigevent_t`, registration pool, waiter kinds |
| `tests/unit/test_pulse.c` | 22 test groups |

---

## 2. Results

```
test_objects:   325 checks, 0 failure(s)
test_ipc:    120149 checks, 0 failure(s)
test_pulse:   21351 checks, 0 failure(s)
```

| Behaviour | Result |
|---|---|
| `MsgSendPulse(-chid, …)` to the caller's own channel | received on that channel, `scoid == 0` |
| Negative coid that names no channel | `ESRCH` |
| Blocked sibling thread woken by a local pulse | `READY`, code and priority preserved |
| `MsgReceivePulse` with 23-byte buffer, pulse queued | `EFAULT`, pulse destroyed |
| `MsgReceivePulse` with a short buffer that had to block | blocks, then `EFAULT` and the pulse is lost |
| Message queued, `MsgReceivePulse` waits | blocks; a later pulse reaches it; the message survives for `MsgReceive` |
| Three pulses at priorities 10/30/20 | received 30 → 20 → 10 |
| `priority == -1` / `== 0` | caller's priority / coerced to 1 |
| Code `0xFF` | reads back `-1` (8-bit wire field) |
| Disconnect with two connections | no pulse on the first detach, `_PULSE_CODE_DISCONNECT` on the last |
| Disconnect with the flag clear | no pulse |
| Client process death | kernel detaches, server still notified |
| Channel destroyed with a sender queued | sender `READY` with `ESRCH`, no message leak |
| Channel destroyed with a receiver blocked | receiver `READY` with `ESRCH` |
| Server dies holding an unreplied message | client `READY` with `ESRCH`, no message leak |
| Register → send handle → deliver | client receives the pulse with the registered code/value |
| Delivery and priority | client `eff_prio` and `sticky_prio` unchanged |
| Unregistered event with `rcvid > 0` | `EACCES` |
| Handle presented by the wrong server | `EACCES` (deliver) / `EINVAL` (verify) |
| `MsgVerifyEvent` with an unresolvable pulse target | `ESRCH` |
| `SIGEV_UNBLOCK` to a blocked thread | `READY` with `EINTR`; repeat delivery is a no-op |
| `SIGEV_SIGNAL` registration | `EINVAL` |
| 1024 registrations then one more | `EAGAIN`; process death frees all 1024 |
| 10,000 pulses | all received in order, pool empty |

---

## 3. Semantics verified against the vendor reference, not assumed

Every rule below was checked against the QNX 8.0 C Library Reference or the shipping headers before it was implemented. Three of them contradicted the working assumption.

**Negative coid.** `MsgSendPulse`: "a coid value of -5 means the pulse is sent to channel 5 in the local process." Implemented as `-chid` in the caller's own process for every coid-taking entry point, not just `MsgSendPulse`.

**Short pulse buffers.** `MsgReceivePulse` errors: "the size of the receive buffer is less than the size of a struct `_pulse`. The pulse is lost in this case." The loss applies to the blocking case too: a thread that blocked and then lost its wake-up pulse is exactly the failure mode this requirement exists to pin down.

**`struct _pulse` is 24 bytes on LP64**, and the code field is `int8_t`. `MsgSendPulse` documents the safe code range as `_PULSE_CODE_MINAVAIL.._PULSE_CODE_MAXAVAIL` (0..127), which is where sign-extension stops being observable.

**`struct sigevent` is 32 bytes**, matching the PRD's trace field. `_Static_assert` enforces it.

**`MsgRegisterEvent` replaces the caller's event with a handle.** The library reference: "Calling MsgRegisterEvent() replaces the given event with a handle." So `MsgDeliverEvent` and `MsgVerifyEvent` resolve a handle and check that the delivering server is the one the client registered against. A byte comparison of the original event would reject every real client, because the client sends the handle.

**`_NTO_CHF_DISCONNECT` is about client detach, not channel destruction.** "Deliver a pulse when all connections from a client process on this channel are detached… If a process dies without detaching all its connections, the kernel detaches them for it." Implemented as a post-detach scan, and the process-death path routes through the same function.

**Pulse compression is documented.** "The kernel can compress identical pulses… by updating a 16-bit count, as long as compression preserves the delivery order. Specifically, the most recently queued pulse at the same priority must be identical." This answers PRD open question Q5. Because compression requires the most recent pulse at that priority to be identical, it cannot reorder anything; a per-pulse queue is observationally equivalent. Recorded as IMP-16, no code.

**Pulse codes** (`-32` UNBLOCK, `-33` DISCONNECT, `-34` THREADDEATH, `-35` COIDDEATH) and channel flags come from `sys/neutrino.h`; they are part of the modelled ABI and a wrong number would fail conformance silently.

---

## 4. Deviations from the PRD's wording, and why

| PRD text | Implemented | Reason |
|---|---|---|
| `MsgVerifyEvent`: "Byte compare; `EINVAL` on mismatch" | Handle ownership check plus field comparison of the normalised event | The vendor mechanism replaces the event with a handle (above). The padding words of `struct sigevent` are unspecified, so a literal byte compare would reject valid events on a different compiler |
| FR-3.5 lists five functions only | Also releases registrations on process death | A registry with no release path turns a pool limit into a deterministic false failure |

Both are recorded as `IMP-14`, `C-08` in [`docs/DECISIONS.md`](../docs/DECISIONS.md).

---

## 5. Deliberately not done

| Gap | Why |
|---|---|
| `SIGEV_SIGNAL*`, `SIGEV_THREAD`, `SIGEV_MEMORY` | Signals are excluded by the PRD; registration rejects them loudly |
| `_NTO_REGEVENT_ALLOW_ANY_SERVER` and friends | Their numeric values are not in the reference text. A guessed constant is a fake ABI |
| `_NTO_CHF_COID_DISCONNECT` | Client-side death notification needs per-connection tracking on the client's channel; no requirement pins it |
| `_NTO_CHF_THREAD_DEATH` | The reference does not say which channel receives the pulse |
| `_NTO_COF_NOEVENT` | Connection flags are stored but not enforced; would only add `EACCES` paths with no test |
| Per-channel pulse pools | Single pool; exhaustion is `EAGAIN` either way |
| Timeout-driven cancellation and `_NTO_CHF_UNBLOCK` | Needs the timeout engine (P1-06). The flag is defined, the transition is documented, the behaviour is not implemented |
| `chid == -1` dissociation (server boost) | Needs the server-boost model (P1-05) |

---

## 6. One test was wrong, not the kernel

`test_disconnect_on_last_detach` initially blocked the server in `MsgReceivePulse` and then expected the disconnect pulse to be *queued*. It is delivered into the blocked thread's pending result, which is the same rule `MsgSendPulse` uses for any blocked receiver. The test now asserts the delivery and separately checks that nothing is left in the queue; the queued path is covered by `test_disconnect_on_client_death`.

The same review surfaced that `test_ipc` used pulse code `0x99` (153). The wire field is `int8_t`, so it reads back as `-103`; the test now uses an in-range code and `test_pulse` pins the truncation explicitly with `0xFF` → `-1`. Fixing the test rather than the kernel is the correct call: the truncation is what QNX does.

---

## 7. Next

**P1-04 — ready queue and scheduler.** 256 buckets plus an occupancy bitmap, O(1) pop, FIFO within a priority, `SCHED_FIFO/RR/OTHER`, a four-tick RR quantum, and `SchedYield` that yields only to equal-priority threads. That is the first task where the `CONTINUE`/`BLOCKED` seam from P1-02 finally has an executor behind it.
