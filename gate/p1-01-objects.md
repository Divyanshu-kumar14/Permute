<!-- SPDX-License-Identifier: Apache-2.0 -->

# P1-01 report — kernel object model

**Date:** 2026-11-14
**Commit:** `0dd1f49` + this work
**Scope:** handle tables, object pools, rcvid encoding, payload arena, effective priority, receive queue.
**Result:** **312 checks, 0 failures**, including 10⁶ handle cycles and a clean ASan/UBSan run.

---

## 1. What was built

| Path | Purpose |
|---|---|
| `kernel/state.h` | Pools, handle/rcvid encoding, object structs, `permute_eff_prio` |
| `kernel/objects.h/.c` | Table primitives, process/thread/channel/connection/message/pulse lifecycle, queue |
| `kernel/blob.h/.c` | 12-class payload arena, 4 MiB, free lists threaded through blocks |
| `tests/harness.h` | Minimal assertion harness (Unity lands at P1-15) |
| `tests/unit/test_objects.c` | 12 test groups |

---

## 2. Results

```
test_objects: 312 checks, 0 failure(s)
```

| Check | Result |
|---|---|
| 10⁶ alloc/free cycles, single slot | generation advances by exactly 1 each time, never repeats |
| Stale handle after free | rejected |
| Table exhaustion | `-EMFILE`, freeing one slot restores capacity |
| rcvid encoding | positive at maximum generation and slot; `0` rejected |
| rcvid after message destroy | `-ESRCH` |
| Payload copy (FR-3.7) | source poisoned after send; kernel copy unchanged |
| Per-process chid space | two processes both own chid 0, distinct objects |
| Connection refcount | channel `conn_refs` 1 → 0 on detach; double detach `-ESRCH` |
| Effective priority | base < sticky < mutex < protect; `SchedSet` clears sticky, keeps lock contributions |
| Blob classes | 12 classes, reuse after free, exhaustion returns NULL |
| Queue | FIFO across the full ring, overflow `-EAGAIN` |

ASan/UBSan: clean. Verified the sanitisers are genuinely linked (`libasan.so.8`, `libubsan.so.1`) and that the setup detects a deliberate overflow — a green run from an unlinked sanitiser would prove nothing.

---

## 3. Encoding decisions

### Handles are 64-bit

A 32-bit handle (16-bit generation) wraps after 65 536 frees of the same slot. The acceptance criterion is "generation never repeats", so the generation is 32 bits and the slot another 32. Cost: per-process `connections[1024]` is 8 KiB instead of 4 KiB; 64 processes total 512 KiB. Accepted.

### rcvid is 32-bit with a 15-bit generation

The ABI forces a positive `int32`, so the generation must fit in 15 bits to keep the sign bit clear. That is a real aliasing boundary at 32 768 reuses of one message slot, documented rather than papered over.

The message pool **is** the rcvid space — one table, one generation source. A separate rcvid table would allow the two to disagree, and that class of bug is invisible until replay.

### No malloc in the kernel

All pools are static arrays inside `permute_kernel_t`; the payload arena is a separate 4 MiB static backing store with 12 size classes and free lists threaded through the blocks. A run cannot depend on allocation history because there is no history to depend on.

`permute_kernel_t` is roughly 800 KiB plus the 4 MiB arena. It is intended as a singleton (IMP-13), which is why the arena backing lives outside the struct.

---

## 4. What this does not do yet

| Gap | Why it is deliberate | Task |
|---|---|---|
| Receive queue is insertion-ordered, not priority-ordered | Ordering is IPC semantics, and it needs the scheduler's priority comparison | P1-02 / P1-04 |
| Process death does not release its in-flight messages | Releasing them silently would hide a leak check; the IPC layer owns message lifetime | P1-02 |
| Ceiling clamping not applied in `recompute_prio` | The ceiling lives on the process; clamping belongs at send/receive | P1-05 |
| `prot_ceil` is a single scalar, not a set of held mutexes | The mutex layer computes it on lock/unlock | P1-05 |
| No locking | Single CPU, single owner by design | — |

---

## 5. Next

**P1-02 — IPC core.** Channel/connection message path: `MsgSend` blocks or completes, `MsgReceive` returns `0` for a pulse and a positive rcvid for a message, `MsgReply` copies out and wakes the client, `MsgSendPulse` enqueues without blocking, payload copy at send, `ESRCH` on a destroyed channel. That is the first end-to-end slice where the simulator does something a QNX program would recognise.
