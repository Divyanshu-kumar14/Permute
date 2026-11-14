<!-- SPDX-License-Identifier: Apache-2.0 -->

# Simulator reference traces

These are the committed references `make conformance` compares against when no
QNX capture is present. Each `C*.jsonl` is a canonical trace (one event per
line, sorted by the canonical linear extension) and each `C*.outcome` records
the outcome class.

They were produced by `python3 conformance/runner/run_conformance.py --record`
using the simulator itself. Consequences, stated plainly:

- they guard against regressions in the simulator and exercise the
  canonicaliser and comparator;
- they are **not** evidence about QNX behaviour;
- a scenario that differs from its own previous output fails, which is the
  intended use.

Replace them with QNX captures under `../qnx/`; the runner prefers that
directory whenever `../qnx/manifest.toml` exists.
