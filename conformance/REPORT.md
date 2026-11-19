<!-- SPDX-License-Identifier: Apache-2.0 -->

# Conformance report

**Oracle source:** simulator reference (not a QNX capture)  
**Executed:** 12  **Skipped:** 2  **Failures:** 0  
**Mandatory executed:** 10/12  **Mandatory skipped:** 2

| ID | Behaviour | Result | Detail | Oracle |
|---|---|---|---|---|
| C01 | request/reply, 1,000 messages | ok | 9006 events equivalent; outcome CLEAN | simulator |
| C02 | receive queue ordered 30,20,10 | ok | 48 events equivalent; outcome CLEAN | simulator |
| C03 | FIFO within equal priority | ok | 48 events equivalent; outcome CLEAN | simulator |
| C04 | pulse and message both delivered | ok | 22 events equivalent; outcome CLEAN | simulator |
| C05 | pulse queued while the server processes | ok | 31 events equivalent; outcome CLEAN | simulator |
| C06 | pulse-only waiter ignores a message | ok | 41 events equivalent; outcome CLEAN | simulator |
| C07 | timeout, unblock pulse, late reply ESRCH | ok | 41 events equivalent; outcome CLEAN | simulator |
| C09 | reply after the client exited | ok | 29 events equivalent; outcome CLEAN | simulator |
| C10 | cancelled sender leaves no message | ok | 13 events equivalent; outcome CLEAN | simulator |
| C11 | server death with reply-blocked clients | skip | the simulator runs one guest process; server death needs process teardown | - |
| C12 | message PI and FIXED_PRIORITY | ok | 31 events equivalent; outcome CLEAN | simulator |
| C13 | mutex PI chain | skip | the mutex shim is not modelled (LIMITS #37) | - |
| C14 | round-robin alternation | ok | 37 events equivalent; outcome CLEAN | simulator |
| C15 | ChannelDestroy unblocks senders with ESRCH | ok | 29 events equivalent; outcome CLEAN | simulator |

TimerTimeout mask bits: 11/11.

> These references were produced by the simulator itself. They guard against regressions and exercise the canonicaliser and comparator; they are **not** target trace captures. The separate QNX 8.0 build and target run is committed in [`docs/evidence/qnx/`](../docs/evidence/qnx/): 14/14 scenarios cross-built with `qcc` and 14/14 `rc=0` on a booted QNX 8.0 image, each checking its own assertions. Target trace capture is documented in `docs/CONFORMANCE.md`.
