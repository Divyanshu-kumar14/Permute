# SPDX-License-Identifier: Apache-2.0
"""Post-hoc invariant oracles over a decoded trace.

The predicates stream the event log; they never look at the guest. That is what
lets the same code run on simulator traces and on captured on-target traces
(FR-10.1), and it is why every oracle here takes records, not kernel state.

A trace written by the built-in workload has no thread records. Oracles that
need thread state are reported as skipped rather than silently passing.
"""

from .engine import Evaluation, Finding, evaluate, SUPPORTED

__all__ = ["Evaluation", "Finding", "evaluate", "SUPPORTED"]
