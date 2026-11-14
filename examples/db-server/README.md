<!-- SPDX-License-Identifier: Apache-2.0 -->

# db-server

A key/value server with three clients at priorities 5, 10 and 15. Each client
issues six requests plus one deliberate miss; the server answers valid requests
with `MsgReply` and failures with `MsgError`.

## Run

```sh
make            # at the repository root, once
cd examples/db-server
../../tools/permute build
../../tools/permute run --guest .permute/build/proc_smoke__0.so
../../tools/permute show --seed 0
```

Expected (also in [`expected.txt`](expected.txt)):

```text
3 seeds: 3 clean, 0 crash, 0 budget, 0 violation, 0 internal
seed 0: 251 records, digest f43ea213ae6f3f0f
seed 1: 251 records, digest 3f5f2679699ec71c
seed 2: 260 records, digest c0daddd3603fa1fa
```

## What it demonstrates

- **Seed-dependent workload.** `rand()` selects the operation, key and value.
  The kernel installs the per-run seed table, so each seed produces a different
  request order and a different trace. This is the only example whose digests
  differ across seeds, and it makes seeded exploration visible.
- **Priority inheritance.** The server runs at 10. When a client blocks on its
  reply the server inherits the client's priority; the server reads its own
  effective priority with `SchedGet` and the client checks that the reply
  reports at least its own priority. An inversion would fail the guest and the
  run would not be clean.
- **Error path.** `MsgError(ENOENT)` for a missing key, `EMSGSIZE` for a short
  message, `ENOSPC` when all slots are taken. The client asserts that a delete
  of a never-written key fails with `ENOENT` instead of returning success.
- **Pulses and a deadline.** Each client pulses the main thread when done; the
  main thread receives three pulses under `TimerTimeout(..., RECEIVE, ...)`.
- **Clean shutdown.** The main thread pulses `_PULSE_CODE_DISCONNECT`, the
  server exits its receive loop, and the channel is destroyed.

## Try a change

Edit `scenarios/smoke.c`, then:

```sh
../../tools/permute build
../../tools/permute run --guest .permute/build/proc_smoke__0.so
python3 ../../tools/check_examples.py --update   # new expected output
```

Change `REQUESTS` or a priority and watch the record count and digest move.
Introduce an inversion (serve at priority 20 but reply with `server_prio` 1) and
the run reports `1 internal` instead of `3 clean`.
