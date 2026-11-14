<!-- SPDX-License-Identifier: Apache-2.0 -->

# QNX evidence

**Owner:** [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14)
**Status:** **blocked — no QNX SDP is installed.** Nothing in this file is target
evidence; it records the exact evidence PRD §17 requires, what exists today,
and the procedure that produces the rest. The fields below must not be filled
from simulator output.

---

## 1. Required evidence (PRD §17, row 4)

| Field | Required content | State |
|---|---|---|
| SDP version | `QNX Software Development Platform 8.0`, build id | **not available** — no SDP |
| `uname -a` | target kernel string from the booted image | **not available** |
| Image SHA-256 | hash of the QEMU disk/IFS image used | **not available** |
| QEMU invocation | full `qemu-system-x86_64` command line | **not available** |
| Serial log excerpt | boot plus capture run, timestamps visible | **not available** |
| Commit SHA | the commit the capture agent was built from | **not available** |
| Oracle traces | `conformance/oracle/qnx/C*.jsonl` + `C*.outcome` + `manifest.toml`, captured on QNX | **absent** — [`conformance/oracle/qnx/README.md`](../conformance/oracle/qnx/README.md) only |
| `permute-capture` | built with `qcc -Vgcc_ntox86_64` and run inside QNX 8.0 | **not built** — `qcc` absent |

Until every row is filled, no claim of target fidelity is made. The conformance
report says `oracle: simulator reference (not a QNX capture)` on every run, by
design.

## 2. What exists today

- **Dual-target scenario sources.** All `conformance/scenarios/c*.c` include
  the vendor headers under `__QNXNTO__` and the clean-room ABI otherwise, and
  call `permute_main` from a `main` wrapper on the target.
- **Canonical schema and comparator.** `permute-canonical/1`,
  `conformance/tracecmp.py`, and `conformance/ALLOWED-DIFFERENCES.md` define
  exactly which fields are compared, so a capture can be validated without
  renegotiating the comparison.
- **Oracle selection.** `conformance/runner/run_conformance.py` prefers
  `conformance/oracle/qnx/` when a manifest is present and labels the source in
  every report.
- **Capture procedure.** `docs/CONFORMANCE.md` and
  `conformance/oracle/qnx/README.md` carry the steps, including the manifest
  fields.

## 3. Host-only justification (PRD §17, row 5)

Permute is a **host tool by design**: it runs a service's source against a
simulated microkernel on Linux, and the QNX use is evidenced by the target
twin, not by running the simulator on QNX.

- The simulator itself needs no SDP and no QNX licence (NFR-10); that keeps the
  tool usable in CI and in containers.
- Genuine QNX use is evidenced by three things, all currently **pending**:
  `permute-capture` cross-built with `qcc`, run inside QNX 8.0 under QEMU, and
  the resulting oracle captures committed under `conformance/oracle/qnx/`.
- The ABI declarations are written from public documentation; no vendor headers
  are shipped (`abi/permute_qnx.h` is clean-room, see ADR-011 and
  [`docs/LIMITS.md`](LIMITS.md) §2.3).

## 4. Procedure to fill this file

1. Install SDP 8.0 and a bootable x86_64 QEMU image.
2. Record the SDP build id, `uname -a` from the booted image, the image
   SHA-256, and the exact QEMU command line.
3. Cross-build the capture agent and every `conformance/scenarios/c*.c`:

   ```sh
   qcc -Vgcc_ntox86_64 -o permute-capture <agent sources>
   qcc -Vgcc_ntox86_64 -o c01 c01_request_reply.c
   ```

4. Run each binary inside QNX 8.0; capture the serial log.
5. Commit `C*.jsonl`, `C*.outcome`, and `manifest.toml` under
   `conformance/oracle/qnx/`, then run `make conformance` and confirm the report
   says `oracle: QNX capture`.
6. Replace every **not available** in section 1 with the measured value, name
   the commit SHA of the capture agent, and keep the serial excerpt short but
   sufficient to identify the boot and the run.

## 5. Integrity rules

- A capture is committed only with the manifest that identifies its SDP
  version and QEMU invocation.
- Simulator output is never copied into `conformance/oracle/qnx/`; the runner
  labels whichever directory it used.
- If a capture must be re-taken, the old one is replaced, not averaged: the
  manifest carries one date and one image hash.
