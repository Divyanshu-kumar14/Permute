<!-- SPDX-License-Identifier: Apache-2.0 -->

# P1-02 report — IPC core

**Date:** 2026-11-14
**Commit:** `345df0e` + this work
**Scope:** message path, blocking states, receive-queue ordering, reply/error, basic pulses.
**Result:** **120,149 checks, 0 failures**, including 10,000-message ping-pong, clean ASan/UBSan.

---

## 1. What was built

| Path | Purpose |
|---|---|
| `kernel/ipc.h` | `permute_action_t` contract, `permute_msg_info_t`, entry points |
| `kernel/msg.c` | send / receive / reply / error / send-pulse state machine |
| `kernel/state.h` | threaded receive and reply contexts, typed priority queue, channel waiters |
| `tests/unit/test_ipc.c` | 10 test groups |

The kernel still does not pick the next thread. Every entry point returns `CONTINUE` or `BLOCKED`; the executor (P1-04) owns that decision. This is what allows the state machine to be tested exhaustively without a scheduler — and it is the same interface the hypercall path will use.

---

## 2. Results

```
test_objects: 325 checks, 0 failure(s)
test_ipc:   120149 checks, 0 failure(s)
```

| Behaviour | Result |
|---|---|
| 10,000-message ping-pong, 64-byte payloads | bytes match on every hop; sender `SEND` → `REPLY` → `READY` each round |
| Receiver blocks first, then send | receiver wakes `READY` with the rcvid and a filled buffer |
| Priority ordering (10 / 30 / 20 senders) | received 30 → 20 → 10 |
| Equal priority | insertion order preserved |
| Receive buffer smaller than message | truncated copy, `msglen` reports the full length |
| Reply larger than client buffer | truncated copy, `reply_len` reports what fit |
| `MsgError` | client sees `-1` with the given errno |
| Destroyed channel | send returns `ESRCH`, no message leak |
| Invalid rcvid (`0`, negative, garbage) | `ESRCH` |
| Pulse with no waiter | queued, receive returns **0** (FR-3.3) with code/value |
| Pulse with a blocked receiver | wakes it, `rcvid == 0`, priority preserved |

ASan/UBSan: clean.

---

## 3. Two details that would be wrong the obvious way

### The reply buffer lives on the blocked thread, not in the message

FR-3.7 forbids retaining SUT pointers past `enter()`. But the reply buffer must be reachable when the server replies, which is later by construction. Storing it in the message object would be exactly the retained pointer the rule forbids.

The resolution: the pointer belongs to the *blocked client thread's context* (`reply_buf`), not to the message. The message holds only copied payload. The distinction matters because the thread context is destroyed when the thread dies, whereas a message can outlive a dead client — and that case must produce `ESRCH`, not a write into freed memory.

### `SEND` becomes `REPLY` at receive, not at reply

A client blocked in `MsgSend` is in `SEND` while the message is queued. It moves to `REPLY` the moment a server *receives* the message. The client is blocked in both states, but they differ for cancellation, unblock pulses, and timeout masking — all of which arrive in P1-03/P1-06. Modelling it as "blocked until reply" would work for ping-pong and fail every conformance case that cares about the difference.

---

## 4. Ordering

The receive queue is a sorted array (capacity 64): priority descending, insertion sequence ascending. Channel waiters use the same structure ordered by effective priority, so the highest-priority blocked receiver is woken first.

Linear insert is the right call at 64 entries: it is branch-predictable, allocation-free, and its worst case is smaller than a heap's average case at this size. If the queue depth ever grows, this is the first thing to revisit.

---

## 5. What this does not do yet

| Gap | Owner |
|---|---|
| No executor: nobody actually resumes a `READY` thread | P1-04 |
| No priority boost on send; `sticky_prio` is never raised | P1-05 |
| No `_NTO_CHF_FIXED_PRIORITY`, no ceiling clamp | P1-05 |
| Timeouts never fire; `timeout_mask` is carried but unused | P1-06 |
| `MsgDeliverEvent`, `MsgRegisterEvent`, negative-coid pulses | P1-03 |
| Client death during `SEND`/`REPLY` does not unblock peers | P1-03 |
| No trace records emitted from the message path | P1-07 |

---

## 6. Next

**P1-03 — pulses and events.** Negative coid addressing the local channel, `MsgReceivePulse` short-buffer `EFAULT` with drop, `MsgDeliverEvent` requiring prior registration and not boosting priority, and client-death unblock with `_NTO_CHF_UNBLOCK`. Then P1-04 gives the executor something to schedule.
