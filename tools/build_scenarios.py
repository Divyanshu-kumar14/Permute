#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build every scenario and check the coverage each one promises.

Compilation is necessary but not sufficient: the fanout must touch every
modelled primitive, and s03 must name all eleven TimerTimeout mask bits. Those
are properties of the source, so they are checked statically here rather than
assumed from a successful link.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
BUILD = REPO / "build"
OUT = BUILD / "scenarios"

TIMEOUT_BITS = [
    "_NTO_TIMEOUT_SEND",
    "_NTO_TIMEOUT_RECEIVE",
    "_NTO_TIMEOUT_REPLY",
    "_NTO_TIMEOUT_SIGSUSPEND",
    "_NTO_TIMEOUT_SIGWAITINFO",
    "_NTO_TIMEOUT_NANOSLEEP",
    "_NTO_TIMEOUT_MUTEX",
    "_NTO_TIMEOUT_CONDVAR",
    "_NTO_TIMEOUT_JOIN",
    "_NTO_TIMEOUT_INTR",
    "_NTO_TIMEOUT_SEM",
]

FANOUT_PRIMITIVES = [
    "ChannelCreate", "ConnectAttach", "MsgSend", "MsgReceive", "MsgReply",
    "MsgError", "MsgSendPulse", "MsgReceivePulse", "MsgRegisterEvent",
    "MsgDeliverEvent", "MsgVerifyEvent", "TimerTimeout", "nanosleep",
    "SchedSet", "SchedGet", "SchedYield", "ChannelDestroy", "ConnectDetach",
    "pthread_create", "pthread_cancel",
]

failures = 0


def fail(message: str) -> None:
    global failures
    failures += 1
    print(f"FAIL {message}")


def scenarios() -> list[tuple[str, Path]]:
    found = []
    for parent in ("scenarios", "examples"):
        root = REPO / parent
        if not root.is_dir():
            continue
        for source in sorted(root.glob("*/scenario.c")):
            found.append((source.parent.name, source))
    return found


def check_source(name: str, source: Path) -> None:
    text = source.read_text()
    if "permute_main" not in text:
        fail(f"{source}: no permute_main")

    if name == "telemetry":
        missing = [p for p in FANOUT_PRIMITIVES if p not in text]
        if missing:
            fail(f"{source}: fanout is missing {', '.join(missing)}")
        else:
            print(f"  fanout covers all {len(FANOUT_PRIMITIVES)} primitives")

    if name == "s03_timer_timeout":
        missing = [b for b in TIMEOUT_BITS if b not in text]
        if missing:
            fail(f"{source}: s03 is missing {', '.join(missing)}")
        else:
            print(f"  s03 covers all {len(TIMEOUT_BITS)} timer mask bits")


def build(name: str, source: Path) -> None:
    result = subprocess.run(
        [sys.executable, str(REPO / "tools" / "make_guest.py"),
         "--source", str(source), "--name", name, "--instance", "0",
         "--out", str(OUT), "--build-dir", str(BUILD)],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        fail(f"{source}: build failed\n{result.stdout}{result.stderr}")
        return
    print(f"  built {result.stdout.strip()}")


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)

    found = scenarios()
    if not found:
        fail("no scenarios found")

    for name, source in found:
        print(f"{name}:")
        check_source(name, source)
        build(name, source)

    print(f"scenarios: {len(found)} built, {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
