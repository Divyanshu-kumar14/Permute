#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Header-gap report.

Lists the modelled interposition surface and says, plainly, what is not
verified without a vendor toolchain. `permute init` writes this next to the
symbol audit so a new project starts with an honest statement of coverage.
"""

from __future__ import annotations

import re
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEF = REPO / "abi" / "permute_iface.def"

SYMBOL_RE = re.compile(
    r"PERMUTE_SYMBOL\(\s*(\w+),\s*([^,]+),\s*(\w+),\s*(\([^)]*\)),\s*[^)]*\)"
)

# The groups the full surface is divided into (ADR-001). Only the ones with
# symbols today are listed in the table; the rest are named as gaps.
ALL_GROUPS = ("ipc", "sched", "pthread", "time", "proc", "fs", "stdio")


def symbols() -> list[tuple[str, str, str, str]]:
    text = "\n".join(line.split("//", 1)[0]
                     for line in DEF.read_text().splitlines())
    return [(m.group(1), m.group(2).strip(), m.group(3), m.group(4))
            for m in SYMBOL_RE.finditer(text)]


def render() -> str:
    rows = symbols()
    groups = Counter(group for group, *_ in rows)

    lines = [
        "<!-- SPDX-License-Identifier: Apache-2.0 -->",
        "",
        "# Header gaps",
        "",
        f"Modelled surface: **{len(rows)} symbols** across "
        f"**{len(groups)} groups**.",
        "",
        "| Group | Return | Symbol | Parameters |",
        "|---|---|---|---|",
    ]
    for group, ret, name, params in rows:
        lines.append(f"| `{group}` | `{ret}` | `{name}` | `{params}` |")

    missing = [g for g in ALL_GROUPS if g not in groups]
    lines += [
        "",
        "## What is not verified here",
        "",
        "- No vendor header is shipped (ADR-011). These prototypes were written",
        "  from public documentation, so a signature can drift if the vendor",
        "  changes one. Verifying them requires `--qnx-target` and a QNX SDP.",
        "- Groups not yet interposed: "
        + (", ".join(f"`{g}`" for g in missing) if missing else "none")
        + ".",
        "- Behavioural conformance is not a header property. It is measured by",
        "  the conformance scenarios against a recorded target trace.",
        "",
        "Regenerate with `python3 tools/header_gap.py`.",
        "",
    ]
    return "\n".join(lines)


if __name__ == "__main__":
    print(render(), end="")
