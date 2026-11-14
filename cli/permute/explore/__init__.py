# SPDX-License-Identifier: Apache-2.0
"""Novelty-guided exploration (FR-5.4 through FR-5.8)."""

from .engine import Entry, Result, explore, parse_shard, subprocess_runner
from .mutate import OPERATORS, Tape, apply

__all__ = ["Entry", "Result", "explore", "parse_shard", "subprocess_runner",
           "OPERATORS", "Tape", "apply"]
