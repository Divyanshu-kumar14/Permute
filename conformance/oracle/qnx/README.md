<!-- SPDX-License-Identifier: Apache-2.0 -->

# QNX captures (external input)

No QNX capture is committed. Producing one requires the QNX 8.0 SDP and a
bootable QEMU image, both external to this repository.

To add one:

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
regression check, not target fidelity.
