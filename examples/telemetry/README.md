<!-- SPDX-License-Identifier: Apache-2.0 -->

# telemetry

The sensor fan-out: one server, three clients at priorities 5, 15 and 25, a
collector that receives pulses, a sampling timer, a client timeout and a client
cancellation. It is the broadest example — the one meant to reach every
modelled primitive.

## Run

```sh
make            # at the repository root, once
cd examples/telemetry
../../tools/permute build
../../tools/permute run --guest .permute/build/proc_smoke__0.so
../../tools/permute show --seed 0
```

Expected (also in [`expected.txt`](expected.txt)):

```text
3 seeds: 3 clean, 0 crash, 0 budget, 0 violation, 0 internal
seed 0: 715 records, digest f18da9337aaca0d1
```

## What it demonstrates

| Primitive | Where |
|---|---|
| `ChannelCreate` with `_NTO_CHF_DISCONNECT` / `_NTO_CHF_UNBLOCK` | `permute_main` |
| `ConnectAttach`, `ConnectDetach`, `ChannelDestroy` | main and each client |
| `MsgSend`, `MsgReceive`, `MsgReply` | clients and server |
| `MsgError(EINVAL)` | one malformed client request |
| `MsgRegisterEvent`, `MsgVerifyEvent`, `MsgDeliverEvent` | a client registers a pulse event; the server delivers it |
| `MsgReceivePulse` | the collector, and the server's timeout pulses |
| `TimerTimeout(_NTO_TIMEOUT_RECEIVE, ...)` | the server's sampling deadline |
| `SchedSet`, `SchedGet`, `SchedYield` | priorities 5/15/25 and cooperative yields |
| `pthread_create`, `pthread_join`, `pthread_cancel` | fan-out and the mid-send kill |
| `nanosleep` | the short run before the kill |

A clean run means all of it behaved: 60 messages sent, 59 replied, 59 events
delivered, one request rejected, the cancelled client joined, and both channels
destroyed.

## Reading the trace

```sh
../../tools/permute show --seed 0 --format ndjson | grep -c '"kind": "SEND"'
```

The event mix is the workload's fingerprint. The digest is identical across
seeds because the priorities are fixed and nothing calls `rand()`; seed
variation is shown by [`db-server`](../db-server/).
