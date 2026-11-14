<!-- SPDX-License-Identifier: Apache-2.0 -->

# Examples

Three runnable workloads, each a complete Permute project. Every one is run by
`make examples` from a clean copy and its output compared with the committed
`expected.txt`, so an example cannot drift from the CLI that runs it.

| Example | Primitives | Trace size | Seed changes the trace? |
|---|---|---|---|
| [`hello-ipc`](hello-ipc/) | `ChannelCreate`, `ConnectAttach`, `MsgSend`, `MsgReceive`, `MsgReply`, `ConnectDetach`, `ChannelDestroy` | 42 records | no — no seed-dependent choice |
| [`db-server`](db-server/) | all of the above plus `MsgError`, `MsgSendPulse`, `MsgReceivePulse`, `TimerTimeout`, `SchedSet`, `SchedGet`, `SchedYield`, seeded `rand()` | 251–260 records | yes |
| [`telemetry`](telemetry/) | all of the above plus `MsgRegisterEvent`, `MsgVerifyEvent`, `MsgDeliverEvent`, `pthread_cancel`, disconnect notification | 715 records | no — fixed priorities, no `rand()` |

Run any of them the same way:

```sh
cd examples/hello-ipc
../../tools/permute build
../../tools/permute run --guest .permute/build/proc_smoke__0.so
../../tools/permute show --seed 0
```

`permute build` compiles `scenarios/smoke.c` into a guest object; `permute run`
without `--guest` runs the built-in seeded workload instead (LIMITS #26). The
examples are guests because the point is to run IPCs you wrote.

## What "expected output" means

A guest cannot print: it runs inside the worker process, and its only visible
product is the trace. Each guest therefore checks its own protocol — reply
payloads, errno values, effective priority, pulse counts — and returns non-zero
if any check fails. The expected output is the run verdict plus the pinned
trace size and digest per seed:

```text
seeds: 3 clean, 0 crash, 0 budget, 0 violation, 0 internal
seed 0: 42 records, digest 1b94fa6f7e644dce
```

A clean run means the workload's own assertions passed **and** the trace is the
one committed here. The digest changing without a source change is a
determinism bug, which is exactly the class of bug this harness exists to
catch.

`make examples` reruns all three; `python3 tools/check_examples.py --update`
rewrites `expected.txt` after an intentional change.
