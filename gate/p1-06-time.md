<!-- SPDX-License-Identifier: Apache-2.0 -->

# P1-06 report — timeouts and the virtual clock

**Date:** 2026-11-14
**Scope:** `TimerTimeout` state masks and the non-matching latch, disarm with the previous deadline, POSIX timers on a shared heap, idle advancement, the deadlock outcome, and `_NTO_CHF_UNBLOCK` emission.
**Result:** **191 checks, 0 failures**, clean ASan/UBSan, full `make test` green (all six suites).

---

## 1. What was built

| Path | Purpose |
|---|---|
| `kernel/vclock.h` | Clock and heap API, simulation outcome enum |
| `kernel/eventq.c` | Fixed 512-entry binary min-heap keyed `(deadline, sequence)` |
| `kernel/vclock.c` | Monotonic nanosecond counter |
| `kernel/timeout.c` | `TimerTimeout`, activation on block, the four unblock paths, `sim_advance`/`sim_run` |
| `kernel/timer.c` | POSIX `timer_create`/`settime`/`delete` on the same heap |
| `kernel/objects.c` | `permute_thread_block` / `permute_thread_wake` (see §3) |
| `tests/unit/test_time.c` | 17 test groups |

---

## 2. Results

```
test_objects:   326 checks, 0 failure(s)
test_ipc:    120149 checks, 0 failure(s)
test_pulse:   21351 checks, 0 failure(s)
test_sched:     277 checks, 0 failure(s)
test_pi:        232 checks, 0 failure(s)
test_time:      191 checks, 0 failure(s)
```

| Behaviour | Result |
|---|---|
| Arm for `RECEIVE`, block in `RECEIVE` | fires at the deadline, `ETIMEDOUT` |
| Arm for `RECEIVE`, block in `SEND` | no timer activates; the intent survives the completed call and fires on the next `RECEIVE` (FR-3.22) |
| Disarm with `flags == 0` | previous deadline returned |
| Disarm with `ntime == NULL` | previous deadline returned (FR-3.23) |
| Arm twice | only the latest deadline fires |
| `SEND` timeout | client `ETIMEDOUT`, queued message destroyed, server receives nothing |
| `REPLY` timeout with `_NTO_CHF_UNBLOCK` | client stays blocked; server receives `_PULSE_CODE_UNBLOCK` with the rcvid; its reply completes the send |
| `REPLY` timeout without the flag | client `ETIMEDOUT`; the late reply returns `ESRCH` |
| `MUTEX` timeout | waiter removed, owner's boost recomputed to zero |
| POSIX one-shot | one pulse, then `DEADLOCK` |
| POSIX periodic | three pulses at 5/10/15 ms |
| `timer_settime` replacement | fires at 9 ms, not the superseded 5 ms |
| `timer_settime(0)` / `timer_delete` | no delivery; `DEADLOCK` |
| `SIGEV_SIGNAL` timer | `ENOTSUP` |
| Empty ready queue and empty heap | `DEADLOCK`, never a hang |
| 10 000 random heaps | every drain is ordered, counts match, heap invariant holds |
| Clock backwards | `EINVAL`, clock unchanged |
| Far-future timer | clock jumps straight to it in one step |

---

## 3. Structural change: block and wake are now one pair

P1-04 shipped with a boundary: IPC transitions set thread state but did not move threads in or out of the ready queue, because there was no executor. That boundary made a timed-out thread structurally unpickable — it would be `READY` but invisible to the scheduler.

This task replaces it with two functions used by every blocking entry point:

- `permute_thread_block(k, t, state, wobj)` — clears `running` if this thread held it, removes it from the ready queue, sets state and wait object.
- `permute_thread_wake(k, t, ret, err, at_head)` — sets `READY`, fills the pending result, re-adds to the queue, and consumes the timeout **only if the state being left was covered by the mask**.

That last clause is what makes FR-3.22 work: a call that completes in a non-matching state leaves the timeout armed for the next matching block; a call that completes in a matching state consumes it.

Recorded as IMP-23, superseding IMP-19. `ARCHITECTURE.md` §6 was updated.

---

## 4. A real bug the tests caught

`test_posix_timer_settime_replaces_and_disarms` failed on the first run: after arming at 5 ms and replacing with 9 ms, the clock advanced to **5 ms**.

The cause was lazy invalidation done incompletely. Superseded heap entries were correctly skipped when they were *fired*, but `sim_advance` still moved the clock to their deadline before discovering they were stale. Replacing a timer was therefore observable in virtual time — and virtual time is recorded.

Fix: before advancing, purge stale entries from the top of the heap. Stale means the target is gone, disarmed, or its sequence no longer matches. Only the top can influence the next jump, so the purge is O(k) in the number of stale entries and no index is needed.

This is exactly the class of defect the model exists to expose, and it only appeared because the test asserted the *clock value* rather than just the delivery. IMP-24.

---

## 5. Two PRD-versus-vendor conflicts, both resolved in the PRD's favour

**C-09 — disarm.** FR-3.23 says `ntime == NULL` disarms and returns the previous deadline. QNX 8.0 says `ntime == NULL` means "no timer; the next blocking state returns `ETIMEDOUT` immediately" — the documented polling idiom — and `flags == 0` is the disarm. Both are implemented: `flags == 0` and `ntime == NULL` disarm; the polling idiom needs the shim to translate `ntime == NULL` to a zero-length timeout.

**C-10 — the latch.** FR-3.21/3.22 require an armed timeout to survive a non-matching blocking call. QNX 8.0 says the next kernel call disarms it when it exits, whatever state it blocked in. The PRD is explicit twice and its acceptance test depends on it, so the latch is implemented; the vendor rule is recorded as divergence #14. The two agree whenever the timeout is armed immediately before the call being timed, which is the documented usage.

Both are in `LIMITS.md` with the exact citations a reviewer needs to disagree.

---

## 6. The unblock contract

For a client in `REPLY` on a channel created with `_NTO_CHF_UNBLOCK`, the timeout does **not** unblock it. The kernel sends `_PULSE_CODE_UNBLOCK` (value = rcvid) to the server, and the client stays blocked until the server replies or errors. Without the flag, the client is unblocked with `ETIMEDOUT` and the message is destroyed, so the server's late reply fails with `ESRCH`.

Both paths are tested, including the server's reply completing the original send after the unblock pulse.

---

## 7. Boundary

| In scope | Out of scope |
|---|---|
| The timeout engine, heap, clock, outcome, unblock paths | `nanosleep`/`clock_nanosleep` entry points (the state bit and plumbing exist; the shim call lands with the time group) |
| POSIX timers with pulse/unblock notification | Signal-notification timers (`ENOTSUP`) and timer tolerance (rejected) |
| One-step idle advancement | The executor loop that consumes `permitted_sim_advance`; that is the run loop |

A stale `wmsg` cannot outlive its thread: teardown frees the message and clears the field, and the heap entry is invalidated by the handle generation.

---

## 8. Next

**P1-07 — trace codec.** `.pmt` header, 64 KiB chunks with CRC32C, the BLAKE3-64 hash chain, `run_digest`, the record kinds, and NDJSON/Chrome export. The event kinds for time (`TIMER_ARM`, `TIMER_FIRE`, `UNBLOCK`) now have something real to describe.
