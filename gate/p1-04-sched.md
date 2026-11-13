<!-- SPDX-License-Identifier: Apache-2.0 -->

# P1-04 report — ready queue and scheduler

**Date:** 2026-11-14
**Scope:** 256-bucket ready queue with occupancy bitmap, FIFO within a priority, `SCHED_FIFO/RR/OTHER`, four-tick RR quantum, `SchedYield` restricted to equal priority.
**Result:** **277 checks, 0 failures**, including a 10⁶-operation model comparison, clean ASan/UBSan, full `make test` green.

---

## 1. What was built

| Path | Purpose |
|---|---|
| `kernel/readyq.h/.c` | The queue: buckets, bitmap, intrusive links, the `check` invariant |
| `kernel/sched.h/.c` | Policy: add/remove/move, pick, tick, yield, running handle |
| `kernel/state.h` | `permute_readyq_t`, `PERMUTE_TICK_NS`, `PERMUTE_RR_QUANTUM_NS`, `quantum_left_ns`, `running` |
| `kernel/objects.c` | Create/destroy maintain membership; `set_prio` re-places a queued thread |
| `tests/unit/test_sched.c` | 13 test groups |

---

## 2. Results

```
test_objects:   326 checks, 0 failure(s)
test_ipc:    120149 checks, 0 failure(s)
test_pulse:   21351 checks, 0 failure(s)
test_sched:     277 checks, 0 failure(s)
```

| Behaviour | Result |
|---|---|
| 10⁶ random push/push-head/remove/pop operations | every pop matches an independent shadow model (max priority, earliest sequence); invariant holds every 1024 operations and at the end |
| FIFO within a priority | insertion order |
| Priority order | 30 → 20 → 10 |
| Head insertion | jumps equal-priority peers |
| Remove from the middle | remaining links intact |
| `-EEXIST` / `-ENOENT` / `-EINVAL` | double push, unknown removal, priority 256 and slot ≥ capacity |
| Three threads at 10/20/30 | picked 30 → 20 → 10, then none |
| RR, 2 threads at the same priority | A for 3 ticks, B on the 4th, then A, then B |
| `SCHED_OTHER` | identical to RR |
| RR with one thread | 50 full quanta, no switch, `cpu_ns` exact |
| FIFO at 20 with an RR thread at 10 ready | 100 quanta, no switch |
| Yield with only lower-priority threads ready | returns self (FR-3.19) |
| Yield with an equal-priority peer | switches to it |
| Yield alone | no-op |
| Destroy a queued thread | removed, count decremented, invariant holds |
| Destroy the running thread | `running` cleared, invariant holds |
| `SchedSet` on a queued thread | moves buckets; at-head placement verified |

---

## 3. Why the pop is O(1), and why that is inspected rather than timed

The pop does exactly three things:

1. Walk the bitmap from word 3 down to word 0 (four iterations, fixed).
2. `63 - __builtin_clzll(word)` for the highest set bit.
3. Take `head[priority]` and unlink it.

There is no loop over threads, no loop over priorities, and no allocation. Removal from the middle is also O(1) because each slot records its own bucket, so unlinking does not search for it.

The structural claim is backed by the invariant, not by a benchmark: `permute_readyq_check` walks every bucket, recomputes each count, compares it with `count[prio]`, compares the bitmap bit with occupancy, checks `tail` against the last link, checks the total, and verifies that unqueued slots have no stale links. A broken bitmap cannot survive a single call, and the random test calls it ~1000 times over the 10⁶-operation sequence.

---

## 4. Yield: one code path instead of a special case

`SchedYield` is "requeue self at the tail of its own bucket, then pick".

- If an equal-priority peer is waiting, it is at the head of the bucket and wins.
- If self is alone at that priority, self is the head and `pick` returns it, so nothing observable happens.
- A lower-priority thread can never win, because `pick` starts at the highest non-empty bucket — and self's bucket is non-empty because self was just pushed into it.

That is FR-3.19 exactly, and it falls out of the data structure rather than a comparison that could be written the wrong way round. The same mechanism gives RR expiry the right behaviour for free: an expired thread is requeued at the tail, so a peer at the same priority runs, and a lone thread keeps running with a fresh quantum.

---

## 5. Boundary of this task

| In scope | Out of scope |
|---|---|
| Queue structure, policies, quantum, yield | Moving threads in/out of the queue on IPC block/wake |
| Membership maintained by create/destroy and `SchedSet` | The executor loop that drives `pick`/`tick` |
| A `running` handle that the executor will own | Preemption decisions triggered by IPC events |

The IPC transitions in `msg.c` and `pulse.c` still set thread state without touching the ready queue. That is deliberate: there is no executor in the plan before the run loop, so wiring them now would add six untestable transitions that would have to be rewritten. It is recorded as IMP-19 and in `ARCHITECTURE.md` §6, so it cannot be mistaken for an oversight.

One consequence is visible in the unit tests: a thread created by the kernel is queued while its state is `READY`, and the tests that only exercise IPC leave it there. `permute_sched_check` therefore validates queue internals and the running/queued exclusion, and does not assert "queued ⇒ `READY`" until the executor owns the transitions.

---

## 6. A test was wrong, not the scheduler

The first run failed the at-head placement check because the test left a priority-20 thread queued and then expected a priority-10 thread to win the pick. The scheduler was right. The test now uses a clean kernel for that sub-case.

---

## 7. Next

**P1-05 — priority model.** Sticky message-driven inheritance (raise at send, assign at receive as the maximum over pending messages, no revert on reply), ceiling clamping, `_NTO_CHF_FIXED_PRIORITY`, and reverting mutex PI. The ready queue's `move` is what makes a receive-time priority change visible to the scheduler, and the RR/yield tests above are the baseline it must not disturb.
