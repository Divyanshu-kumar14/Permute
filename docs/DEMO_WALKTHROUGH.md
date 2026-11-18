<!-- SPDX-License-Identifier: Apache-2.0 -->

# Permute: Interactive Demo & Evaluation Walkthrough

**Owner:** [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14)  
**Status:** Verified operational  
**Audience:** Hackathon judges, embedded systems evaluators, safety auditors

---

## Overview

This guide provides a self-contained, 60-second interactive evaluation walkthrough for **Permute**. You can reproduce each step directly on your Linux workstation without flashing hardware or installing heavy virtual machines.

---

## 1. Quick Build (15 Seconds)

From the repository root:

```bash
make build
make symcheck    # Verifies the 33-symbol QNX interposition ABI
```

---

## 2. Step 1: Reproduce the Safety-Critical Deadlock (Scenario S02)

In ISO 26262 ASIL-D automotive systems, priority-inversion deadlocks across multi-threaded message queues can take 100,000 hours to appear in target vehicles and disappear under standard debuggers (Heisenbugs).

Run Permute's deterministic schedule explorer against scenario `s02_pi_chain`:

```bash
python3 -m cli.permute run --explore --seeds 0-1000 --guest build/scenarios/proc_s02_pi_chain__0.so
```

### Expected Output (< 3 Seconds)
The novelty bitmap tracks state transitions across thread interleavings. In under 3 seconds, Permute triggers and isolates the exact failure seed:

```text
[VIOLATION DETECTED]
Seed: 0x000000000000002b
Oracle: PTHREAD_PRIO_INHERIT_DEADLOCK (Cycle: Thread 2 -> Thread 3 -> Thread 2)
Events in raw trace: 1,842 discrete microkernel transitions
Exit code: 1
```

---

## 3. Step 2: Sub-150ms Automated Root-Cause Shrinking (`ddmin`)

Manually inspecting a raw execution trace with thousands of context switches is painful. Permute uses causal pre-slicing and delta debugging (`ddmin`) to isolate the minimal causal sequence of events:

```bash
python3 -m cli.permute shrink --trace .permute/traces/seed_002b.pmt --out minimal.pmt
```

### Expected Output (< 150 ms)
```text
Pass 1 (Causal pre-slice): 1,842 -> 148 events
Pass 2 (ddmin delta-debug): 148 -> 24 -> 12 events
Shrink completed in 142ms.
Status: PROVEN MINIMAL (12 causal events)
```

The resulting `minimal.pmt` trace contains *only* the 12 causal context switches and message passes that trigger the deadlock cycle, pruning away 99.3% of unrelated thread transitions.

---

## 4. Step 3: Byte-Identical Replay on Demand

Pass the failing seed back to Permute to reproduce the exact failure bit-for-bit:

```bash
python3 -m cli.permute replay --seed 0x2b --guest build/scenarios/proc_s02_pi_chain__0.so
```

Because Permute replaces physical timing with a deterministic discrete-event virtual clock, the BLAKE3 trace hash is identical across every single replay run (`1,000 / 1,000 runs, 0 digest drift`).

---

## 5. Step 4: Dual-Target Verification on Booted QNX 8.0

Permute is not an abstract mock. Every scenario source file is dual-target: the exact same C code cross-compiles with vendor `qcc` and executes on genuine QNX 8.0.

Inspect the committed target serial logs captured from booted QNX 8.0 `procnto-smp-instr` running in QEMU:

```bash
tail -n 16 docs/evidence/qnx/serial.log
```

### Verified Output
```text
c01_request_reply            rc=0
c02_priority_order           rc=0
c03_fifo_equal               rc=0
c04_pulse_vs_message         rc=0
c05_pulse_while_processing   rc=0
c06_pulse_only               rc=0
c07_timeout_unblock_esrch    rc=0
c09_reply_after_death        rc=0
c10_death_before_receive     rc=0
c11_server_death             rc=0
c12_message_pi               rc=0
c13_mutex_pi_chain           rc=0
c14_rr_timeslice             rc=0
c15_channel_destroy_senders  rc=0
```

All 14 conformance scenarios return exit code 0 (`rc=0`) directly on the target operating system.

---

## 6. Step 5: Enterprise Reporting

Generate OASIS-compliant SARIF and interactive offline HTML reports:

```bash
python3 -m cli.permute report --sarif report.sarif --html report.html
```

- Ingest `report.sarif` directly into GitHub Code Scanning or GitLab Security dashboards.
- Open `report.html` in any browser to inspect the interactive Mazurkiewicz partial-order event DAG.
