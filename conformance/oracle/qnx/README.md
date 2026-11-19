<!-- SPDX-License-Identifier: Apache-2.0 -->

# QNX captures (external input)

No QNX **oracle trace** is committed. Producing one requires the QNX 8.0 SDP,
a bootable QEMU image, and the `permute-capture` agent, none of which is part of
a host-only checkout.

This directory and the target run are different evidence:

| Evidence | Location | What it proves |
|---|---|---|
| QNX 8.0 build + target execution | [`docs/evidence/qnx/`](../../../docs/evidence/qnx/) | 14/14 scenarios compile with vendor `qcc` and return `rc=0` on booted `procnto-smp-instr` (scenario self-check, not trace equivalence) |
| Canonical oracle traces | this directory | Would compare simulator and target event sequences under the Mazurkiewicz comparator; not captured yet |

To add an oracle capture:

1. Build each `conformance/scenarios/c*.c` with `qcc` for the target. The
   source selects the vendor headers under `__QNXNTO__` and calls
   `permute_main` from its own `main` wrapper.
2. Run each binary under QEMU with the capture agent, which writes canonical
   JSONL (schema `permute-canonical/1`): normalised ids, payload SHA-256,
   `causal` indices.
3. Write `C*.jsonl` and `C*.outcome` here.
4. Add `manifest.toml`:

   ```toml
   [oracle]
   source = "qnx"
   version = "8.0"
   captured = "2026-11-14"
   qemu = "qemu-system-x86_64 ..."
   ```

5. Run `make conformance`. The runner selects this directory and reports
   `oracle: QNX capture`.

Until this directory has a manifest, every conformance result is a simulator
regression check, not target trace fidelity. The separate QNX 8.0 build and
target run in `docs/evidence/qnx/` is genuine target execution and is reported
as such; it does not substitute for a trace capture.
