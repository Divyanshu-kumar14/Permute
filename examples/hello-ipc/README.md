<!-- SPDX-License-Identifier: Apache-2.0 -->

# hello-ipc

The smallest useful QNX message-passing program: one server thread owns a
channel, one client attaches, and three request/reply round trips run. The
server increments the request value and echoes the struct, so the client checks
the reply value, the derived parity, and the message length.

## Run

```sh
make            # at the repository root, once
cd examples/hello-ipc
../../tools/permute build
../../tools/permute run --guest .permute/build/proc_smoke__0.so
../../tools/permute show --seed 0
```

Expected (also in [`expected.txt`](expected.txt)):

```text
3 seeds: 3 clean, 0 crash, 0 budget, 0 violation, 0 internal
seed 0: 42 records, digest 1b94fa6f7e644dce
```

## What to look at

`permute show --seed 0 --format ndjson` lists the events. The useful ones are
`CHANNEL_CREATE`, `CONNECT_ATTACH`, `SEND`, `RECV`, `REPLY`, `BLOCK`,
`UNBLOCK`, and `SCHED_PICK`.

The digest is identical for every seed because this workload has no
seed-dependent choice: the scheduler is FIFO within a priority and nothing here
calls `rand()` (LIMITS #55). [`db-server`](../db-server/) is the example that
changes with the seed.

`permute_main` is the simulator entry point. The `#ifdef __QNXNTO__` wrapper is
the same source built for the target with `qcc`.
