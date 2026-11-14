<!-- SPDX-License-Identifier: Apache-2.0 -->

# P4-02 report — the example set

**Date:** 2026-11-14
**Scope:** §16.3 — three runnable examples with expected output; each runs from
a clean clone.
**Result:** **PASS.** `hello-ipc`, `db-server` and `telemetry` are complete
Permute projects. `make examples` copies each one into an empty directory,
builds it, runs three seeds, and compares the exact output with the committed
`expected.txt`. All three match on the reference machine in **3.7 s**.

---

## 1. What each example is

| Example | Source lines | Primitives exercised | Seeds 0–2 records |
|---|---|---|---|
| [`hello-ipc`](../../examples/hello-ipc/) | 111 | channel, connect, `MsgSend`/`MsgReceive`/`MsgReply`, detach, destroy | 42 |
| [`db-server`](../../examples/db-server/) | 297 | the above plus `MsgError`, pulses, `TimerTimeout`, `SchedSet`/`SchedGet`/`SchedYield`, seeded `rand()` | 251, 251, 260 |
| [`telemetry`](../../examples/telemetry/) | 196 | the above plus `MsgRegisterEvent`/`MsgVerifyEvent`/`MsgDeliverEvent`, `pthread_cancel`, disconnect notification | 715 |

Each guest checks its own protocol and returns non-zero on a wrong reply, wrong
errno, an inversion, or a lost pulse. A clean run is therefore an assertion
inside the workload, not just a process that exited.

`db-server` is the only example whose output changes with the seed: it calls
`rand()`, which the kernel seeds per run, so the request order and the trace
differ. `hello-ipc` has one request path and `telemetry` has fixed priorities,
so their digests repeat across seeds (LIMITS #55 — the scheduler is FIFO within
a priority).

## 2. The expected output

`expected.txt` is the exact terminal output a reader should see, and the
digests are part of it:

```text
# examples/db-server/expected.txt
seeds: 3 clean, 0 crash, 0 budget, 0 violation, 0 internal
seed 0: 251 records, digest f43ea213ae6f3f0f
seed 1: 251 records, digest 3f5f2679699ec71c
seed 2: 260 records, digest c0daddd3603fa1fa
```

`tools/check_examples.py` parses the run summary and `permute show --json`, so a
digest change with no source change fails the gate — the same signal the
harness exists to catch in a user's workload. `--update` rewrites the file after
an intentional change.

## 3. Verification

```
$ make examples
db-server: PASS (seeds: 3 clean, 0 crash, 0 budget, 0 violation, 0 internal)
hello-ipc: PASS (seeds: 3 clean, 0 crash, 0 budget, 0 violation, 0 internal)
telemetry: PASS (seeds: 3 clean, 0 crash, 0 budget, 0 violation, 0 internal)
examples: PASSED (3 example(s), output matches expected.txt)
```

`make examples` is part of `make ci` (189 s, green), so the examples cannot
drift from the CLI that runs them. A negative test — changing one digest in
`expected.txt` — fails the gate with the expected/actual pair printed; the
checker was added with that comparison path exercised.

## 4. A regression this task caught

`examples/telemetry/scenario.c` was already a mutation target: mutant M10
("`MsgDeliverEvent` boosts the server") is killed by comparing telemetry's
trace. Moving the source to `examples/telemetry/scenarios/smoke.c` (so the
example is a normal project that `permute build` understands) broke
`tools/mutate.py`, and `make gate1` failed at the mutation row with
`cc1: fatal error: …/examples/telemetry/scenario.c: No such file or directory`.
The path was updated, mutation returned to 12/12, and `make gate1` went back to
9/9. The example is load-bearing evidence for G1, not decoration.

## 5. Reproduce

```sh
make
make examples
cd examples/db-server && ../../tools/permute build
../../tools/permute run --guest .permute/build/proc_smoke__0.so
../../tools/permute show --seed 0
```

The output is identical to `examples/db-server/expected.txt`; a different
digest is a determinism bug.
