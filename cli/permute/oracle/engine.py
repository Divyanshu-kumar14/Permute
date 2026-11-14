# SPDX-License-Identifier: Apache-2.0
"""The seven invariant predicates (FR-10.2).

Every predicate is a single pass over the decoded record list. No predicate
touches the guest or the kernel; the trace is the whole input, which is what
makes a finding reproducible from a captured log.
"""

from __future__ import annotations

from dataclasses import dataclass, field

# Oracles that need thread-state records to say anything. The built-in seeded
# workload does not emit them; a guest trace does.
SUPPORTED = {
    "deadlock": ("threads",),
    "unbounded_block": ("threads", "blocks"),
    "priority_inversion": ("threads", "blocks"),
    "starvation": ("threads", "blocks"),
    "queue_depth": ("threads", "blocks"),
    "orphan_reply": (),
    "lost_pulse": (),
}

STATE_NAMES = {
    0: "DEAD", 1: "RUNNING", 2: "READY", 3: "SEND", 4: "REPLY",
    5: "RECEIVE", 6: "NANOSLEEP", 7: "MUTEX", 8: "COND", 9: "SEM",
    10: "JOIN", 11: "INTR", 12: "STOPPED", 13: "ZOMBIE",
}


@dataclass
class Finding:
    invariant: str
    kind: str
    severity: str
    seq: int
    t_ns: int
    detail: str
    classification: str | None = None
    seed: int | None = None

    def as_dict(self) -> dict:
        payload = {
            "invariant": self.invariant,
            "kind": self.kind,
            "severity": self.severity,
            "seq": self.seq,
            "t_ns": self.t_ns,
            "detail": self.detail,
        }
        if self.classification is not None:
            payload["classification"] = self.classification
        if self.seed is not None:
            payload["seed"] = self.seed
        return payload


@dataclass
class Evaluation:
    findings: list[Finding] = field(default_factory=list)
    skipped: list[tuple[str, str]] = field(default_factory=list)

    @property
    def errors(self) -> list[Finding]:
        return [f for f in self.findings if f.severity == "error"]


# ---------------------------------------------------------------------------
# Helpers


def _u32(raw: bytes | None, offset: int = 0) -> int:
    if raw is None or len(raw) < offset + 4:
        return 0
    return int.from_bytes(raw[offset:offset + 4], "little")


def _u64(raw: bytes | None) -> int:
    if raw is None or len(raw) < 8:
        return 0
    return int.from_bytes(raw[:8], "little")


def _text(raw: bytes | None) -> str:
    if not raw:
        return ""
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace")


def _state(record) -> str:
    return STATE_NAMES.get(record.get("aux") or 0, "UNKNOWN")


def _label(tid: int, state: str, obj: int) -> str:
    if state in ("REPLY",):
        return f"T{tid}({state} {obj})"
    if state == "JOIN":
        return f"T{tid}(JOIN)"
    return f"T{tid}({state})"


def _thread_capabilities(records) -> set[str]:
    caps = set()
    for record in records:
        kind = record["kind"]
        if kind == "THREAD_START":
            caps.add("threads")
        elif kind in ("BLOCK", "UNBLOCK", "SCHED_PICK"):
            caps.add("blocks")
    return caps


# ---------------------------------------------------------------------------
# Predicates


def _deadlock(records, invariant, seed) -> list[Finding]:
    live: set[int] = set()
    blocked: dict[int, dict] = {}
    armed: set[int] = set()

    for record in records:
        kind = record["kind"]
        tid = record.get("src") or 0

        if kind == "THREAD_START":
            live.add(tid)
        elif kind == "THREAD_EXIT":
            live.discard(tid)
            blocked.pop(tid, None)
            armed.discard(tid)
        elif kind == "TIMER_ARM":
            armed.add(tid)
        elif kind == "SCHED_PICK":
            armed.discard(tid)
        elif kind == "BLOCK":
            peer = _u32(record.get("blob"))
            blocked[tid] = {
                "state": _state(record),
                "obj": record.get("obj") or 0,
                "peer": peer,
                "seq": record["seq"],
                "t_ns": record["t_ns"],
            }
            armed.discard(tid)
        elif kind == "UNBLOCK":
            blocked.pop(tid, None)
            armed.discard(tid)

    if not live or len(blocked) != len(live):
        return []
    if any(tid in armed for tid in blocked):
        return []  # a timer can still release one of them

    # Each thread waits on at most one peer, so a cycle is a walk that returns
    # to its start. Pick the lowest tid for a stable rendering.
    cycle = None
    for start in sorted(blocked):
        path = [start]
        node = blocked[start]["peer"]
        while node in blocked and node not in path:
            path.append(node)
            node = blocked[node]["peer"]
        if node == start and len(path) >= 2:
            cycle = path
            break
        if node in path:
            begin = path.index(node)
            cycle = path[begin:]
            break

    if cycle is None:
        tid = sorted(blocked)[0]
        return [Finding(
            invariant=invariant["id"], kind="deadlock",
            severity=invariant["severity"], seq=blocked[tid]["seq"],
            t_ns=blocked[tid]["t_ns"],
            detail=f"{len(blocked)} live thread(s) blocked, no wait cycle",
        )]

    labels = [_label(tid, blocked[tid]["state"], blocked[tid]["obj"])
              for tid in cycle]
    rendered = " -> ".join(labels + [f"T{cycle[0]}"])
    return [Finding(
        invariant=invariant["id"], kind="deadlock",
        severity=invariant["severity"], seq=blocked[cycle[0]]["seq"],
        t_ns=blocked[cycle[0]]["t_ns"], detail=rendered,
    )]


def _unbounded_block(records, invariant, seed) -> list[Finding]:
    task = invariant.get("task", "*")
    want_state = invariant.get("state")
    budget_ns = int(invariant["max_virtual_ms"]) * 1_000_000

    names: dict[int, str] = {}
    open_blocks: dict[int, tuple[int, str, int]] = {}
    findings: list[Finding] = []
    last_t = 0

    def close(tid, t_ns, seq):
        entry = open_blocks.pop(tid, None)
        if entry is None:
            return
        start_ns, state, start_seq = entry
        if want_state is not None and state != want_state:
            return
        if task != "*" and names.get(tid, "") != task:
            return
        if t_ns - start_ns <= budget_ns:
            return
        findings.append(Finding(
            invariant=invariant["id"], kind="unbounded_block",
            severity=invariant["severity"], seq=start_seq, t_ns=start_ns,
            detail=f"T{tid} in {state} for "
                   f"{(t_ns - start_ns) / 1_000_000:.1f} ms "
                   f"(limit {invariant['max_virtual_ms']} ms)",
        ))

    for record in records:
        kind = record["kind"]
        tid = record.get("src") or 0
        last_t = record["t_ns"]
        if kind == "THREAD_START":
            names[tid] = _text(record.get("blob"))
        elif kind == "BLOCK":
            open_blocks[tid] = (record["t_ns"], _state(record), record["seq"])
        elif kind in ("UNBLOCK", "THREAD_EXIT"):
            close(tid, record["t_ns"], record["seq"])

    for tid in sorted(open_blocks):
        close(tid, last_t, 0)
    return findings


def _priority_inversion(records, invariant, seed) -> list[Finding]:
    budget_ns = int(invariant["max_virtual_us"]) * 1_000

    picks: dict[int, list[tuple[int, int, int]]] = {}
    pending: dict[int, dict] = {}
    findings: list[Finding] = []

    for record in records:
        kind = record["kind"]
        tid = record.get("src") or 0

        if kind == "SCHED_PICK":
            picks.setdefault(tid, []).append(
                (record["seq"], record["t_ns"], record.get("obj") or 0))
            continue

        if kind == "BLOCK" and _state(record) == "MUTEX":
            # Convention for a mutex block: blob = owner_tid, waiter_prio,
            # inherit_flag. The real mutex shim is not built yet, so this
            # layout is exercised by fixtures and documented in LIMITS.
            pending[tid] = {
                "owner": _u32(record.get("blob"), 0),
                "prio": _u32(record.get("blob"), 4),
                "inherit": _u32(record.get("blob"), 8),
                "seq": record["seq"],
                "t_ns": record["t_ns"],
                "mutex": record.get("obj") or 0,
            }
            continue

        if kind == "UNBLOCK" and tid in pending:
            wait = pending.pop(tid)
            owner = wait["owner"]
            if owner == 0:
                owner = _u32(record.get("blob"))
            end_seq = record["seq"]
            end_ns = record["t_ns"]
            low = [p for p in picks.get(owner, [])
                   if wait["seq"] < p[0] < end_seq and p[2] < wait["prio"]]
            if not low:
                continue
            held_ns = end_ns - wait["t_ns"]
            if held_ns < budget_ns:
                continue

            applied = bool(wait["inherit"])
            findings.append(Finding(
                invariant=invariant["id"], kind="priority_inversion",
                severity=invariant["severity"] if applied else "info",
                classification=("pi_not_applied" if applied
                                else "no_inheritance_configured"),
                seq=wait["seq"], t_ns=wait["t_ns"],
                detail=f"T{tid} (prio {wait['prio']}) waited on mutex "
                       f"{wait['mutex']} owned by T{owner}, which ran at a "
                       f"lower priority for {held_ns / 1000:.0f} us; "
                       + ("inheritance was configured but not applied"
                          if applied else
                          "no inheritance was configured"),
            ))

    return findings


def _starvation(records, invariant, seed) -> list[Finding]:
    limit = int(invariant["max_ready_gap_events"])
    task = invariant.get("task", "*")
    names: dict[int, str] = {}
    present: dict[int, int] = {}
    ready: dict[int, int] = {}
    counters: dict[int, int] = {}
    fired: set[int] = set()
    findings: list[Finding] = []

    for record in records:
        kind = record["kind"]
        tid = record.get("src") or 0

        if kind == "THREAD_START":
            prio = record.get("obj") or 0
            names[tid] = _text(record.get("blob"))
            present[tid] = prio
            ready[tid] = prio
            counters[tid] = 0
            continue
        if kind == "THREAD_EXIT":
            present.pop(tid, None)
            ready.pop(tid, None)
            counters.pop(tid, None)
            continue
        if kind == "BLOCK":
            ready.pop(tid, None)
            counters.pop(tid, None)
            continue
        if kind == "UNBLOCK":
            ready[tid] = present.get(tid, 0)
            counters[tid] = 0
            continue
        if kind != "SCHED_PICK":
            continue

        # A dispatch decision: every ready thread that was passed over while
        # no higher-priority thread was ready has been starved.
        for other in sorted(ready):
            if other == tid:
                continue
            if task != "*" and names.get(other, "") != task:
                continue
            if any(prio > ready[other] for prio in ready.values()):
                counters[other] = 0
                continue
            counters[other] = counters.get(other, 0) + 1
            if counters[other] > limit and other not in fired:
                fired.add(other)
                findings.append(Finding(
                    invariant=invariant["id"], kind="starvation",
                    severity=invariant["severity"], seq=record["seq"],
                    t_ns=record["t_ns"],
                    detail=f"T{other} passed over at more than {limit} "
                           f"dispatch decisions with no higher-priority "
                           f"thread ready",
                ))

        ready.pop(tid, None)
        counters[tid] = 0

    return findings


def _orphan_reply(records, invariant, seed) -> list[Finding]:
    live: dict[int, set[int]] = {}
    findings: list[Finding] = []

    for record in records:
        kind = record["kind"]
        tid = record.get("src") or 0
        rcvid = record.get("obj") or 0

        if kind == "RECV":
            if rcvid > 0:
                live.setdefault(tid, set()).add(rcvid)
        elif kind in ("REPLY", "ERROR"):
            error = record.get("aux") or 0
            known = rcvid in live.get(tid, set())
            if error == 0 and known:
                live[tid].discard(rcvid)
                continue
            if known:
                live[tid].discard(rcvid)
            findings.append(Finding(
                invariant=invariant["id"], kind="orphan_reply",
                severity=invariant["severity"], seq=record["seq"],
                t_ns=record["t_ns"],
                detail=(f"T{tid} replied to rcvid {rcvid} "
                        + ("after the kernel rejected it"
                           if error else "with no receive on record")),
            ))
    return findings


def _lost_pulse(records, invariant, seed) -> list[Finding]:
    findings: list[Finding] = []
    for record in records:
        if record["kind"] == "PULSE_RECV" and (record.get("aux") or 0) == 1:
            findings.append(Finding(
                invariant=invariant["id"], kind="lost_pulse",
                severity=invariant["severity"], seq=record["seq"],
                t_ns=record["t_ns"],
                detail=f"T{record.get('src') or 0} lost a pulse before "
                       f"delivery",
            ))
    return findings


def _queue_depth(records, invariant, seed) -> list[Finding]:
    limit = int(invariant["max_senders"])
    channel = invariant.get("channel", "*")

    depth: dict[int, int] = {}
    seen: dict[int, int] = {}
    findings: list[Finding] = []

    for record in records:
        kind = record["kind"]
        if kind == "BLOCK" and _state(record) == "SEND":
            key = record.get("obj") or 0
            depth[key] = depth.get(key, 0) + 1
            if depth[key] > limit and key not in seen and (
                    channel == "*" or key == channel):
                seen[key] = record["seq"]
                findings.append(Finding(
                    invariant=invariant["id"], kind="queue_depth",
                    severity=invariant["severity"], seq=record["seq"],
                    t_ns=record["t_ns"],
                    detail=f"channel {key} has {depth[key]} blocked senders "
                           f"(limit {limit})",
                ))
        elif kind == "UNBLOCK":
            key = record.get("obj") or 0
            if depth.get(key, 0) > 0:
                depth[key] -= 1
        elif kind == "CONNECT_DETACH":
            key = record.get("obj") or 0
            depth.pop(key, None)
    return findings


_PREDICATES = {
    "deadlock": _deadlock,
    "unbounded_block": _unbounded_block,
    "priority_inversion": _priority_inversion,
    "starvation": _starvation,
    "orphan_reply": _orphan_reply,
    "lost_pulse": _lost_pulse,
    "queue_depth": _queue_depth,
}


def evaluate(records, invariants, seed: int | None = None) -> Evaluation:
    """Run every configured invariant over one trace."""
    result = Evaluation()
    capabilities = _thread_capabilities(records)

    for invariant in invariants:
        kind = invariant["kind"]
        predicate = _PREDICATES.get(kind)
        if predicate is None:
            result.skipped.append((invariant.get("id", kind),
                                   f"unknown oracle kind {kind}"))
            continue

        missing = [need for need in SUPPORTED.get(kind, ())
                   if need not in capabilities]
        if missing:
            words = {"threads": "thread", "blocks": "block"}
            reason = ("trace has no "
                      + " or ".join(words[need] for need in missing)
                      + " records (the built-in workload does not "
                        "instrument threads)")
            result.skipped.append((invariant.get("id", kind), reason))
            continue

        for finding in predicate(records, invariant, seed):
            finding.seed = seed
            result.findings.append(finding)

    return result
