#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Canonical traces and trace comparison (FR-11.2, FR-11.3).

Two runs are equivalent when they describe the same Mazurkiewicz trace: the
same events with the same causal partial order. Independent events may appear
in either order; the canonical linear extension puts them in one.

What is normalised away: thread and object identifiers (assigned by first
appearance), payload bytes (replaced by SHA-256 and a length), and -- unless
`--strict-time` is given -- virtual timestamps. What is kept: event kind,
program order, send -> receive, receive -> reply, the effective priority at a
dispatch, and every errno.

A canonical trace is JSONL, one event per line, with the key `causal` listing
the event indices it depends on. That is enough to rebuild the partial order
without knowing which target produced it.
"""

from __future__ import annotations

import hashlib
import heapq
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path

SCHEMA = "permute-canonical/1"
CANONICAL_FIELDS = ("kind", "src", "dst", "obj", "aux", "payload")


@dataclass(frozen=True)
class Event:
    kind: str
    src: str
    dst: str | None
    obj: object
    aux: int | None
    payload: str | None
    t_ns: int

    def key(self) -> tuple:
        return (self.kind, self.src, self.dst or "", _scalar(self.obj),
                -1 if self.aux is None else self.aux, self.payload or "")

    def as_dict(self, dependencies=None) -> dict:
        payload = {
            "kind": self.kind,
            "src": self.src,
            "dst": self.dst,
            "obj": self.obj,
            "aux": self.aux,
            "payload": self.payload,
            "t_ns": self.t_ns,
        }
        if dependencies is not None:
            payload["causal"] = sorted(dependencies)
        return payload


def _scalar(value):
    if isinstance(value, (int, str)) or value is None:
        return value
    return json.dumps(value, sort_keys=True)


class _Labels:
    """Stable labels, scoped so independent creation order cannot rename them.

    Threads and processes are labelled by first appearance. Objects are also
    scoped by the thread that created or used them, because two independently
    created channels must not swap names when their creation events are
    reordered.
    """

    def __init__(self):
        self.maps: dict[tuple[str, str], dict[object, str]] = {}
        self.counts: dict[tuple[str, str], int] = {}

    def get(self, namespace: str, value, prefix: str) -> str | None:
        if value is None:
            return None
        key = (namespace, prefix)
        table = self.maps.setdefault(key, {})
        if value not in table:
            self.counts[key] = self.counts.get(key, 0) + 1
            number = self.counts[key]
            if prefix in ("t", "p"):
                table[value] = f"{prefix}{number}"
            else:
                table[value] = f"{prefix}@{namespace}:{number}"
        return table[value]


def _payload(blob) -> str | None:
    if blob is None:
        return None
    digest = hashlib.sha256(blob).hexdigest()
    return f"{digest[:32]}:{len(blob)}"


def canonicalise(records) -> tuple[list[Event], list[set[int]]]:
    """Return canonical events and their backward causal edges."""
    labels = _Labels()
    channels: dict[object, str] = {}
    events: list[Event] = []

    for record in records:
        kind = record["kind"]
        src = labels.get("thread", record.get("src") or 0, "t")
        dst = None
        obj = None
        aux = record.get("aux")

        if kind == "THREAD_START":
            obj = record.get("obj")
            aux = None
        elif kind == "CHANNEL_CREATE":
            obj = labels.get(f"ch@{src}", record.get("obj") or 0, "ch")
            channels[record.get("obj") or 0] = obj
        elif kind in ("CONNECT_ATTACH", "CONNECT_DETACH"):
            obj = labels.get(f"co@{src}", record.get("obj") or 0, "co")
            if kind == "CONNECT_ATTACH":
                raw = record.get("aux") or 0
                aux = channels.get(raw) or labels.get(
                    f"ch@{src}", raw, "ch")
                dst = labels.get("process", record.get("dst") or 0, "p")
        elif kind == "SEND":
            obj = labels.get(f"co@{src}", record.get("obj") or 0, "co")
        elif kind in ("RECV", "REPLY", "ERROR"):
            obj = labels.get(f"r@{src}", record.get("obj") or 0, "r")
        elif kind == "PULSE_SEND":
            obj = labels.get(f"co@{src}", record.get("obj") or 0, "co")
        elif kind == "PULSE_RECV":
            obj = record.get("obj")
        elif kind in ("BLOCK", "UNBLOCK"):
            state = aux
            raw = record.get("obj") or 0
            if state == 4:      # REPLY: the rcvid
                obj = labels.get(f"r@{src}", raw, "r")
            elif state == 3:    # SEND: the coid
                obj = labels.get(f"co@{src}", raw, "co")
            elif state == 5:    # RECEIVE: the chid
                obj = labels.get(f"ch@{src}", raw, "ch")
            else:
                obj = raw
            aux = state
        elif kind == "SCHED_PICK":
            obj = record.get("obj")
        elif kind == "FAULT_INJECT":
            obj = record.get("obj")
        else:
            obj = record.get("obj")

        payload = _payload(record.get("blob"))
        if kind == "THREAD_START":
            payload = None  # a process name is host-specific

        events.append(Event(kind, src, dst, obj, aux, payload,
                            record.get("t_ns") or 0))

    # Causal edges, as backward dependencies.
    deps: list[set[int]] = [set() for _ in events]
    previous: dict[str, int] = {}
    recv_by: dict[tuple[str, object], list[int]] = {}
    sends: dict[str, list[int]] = {}
    recvs: dict[str, list[int]] = {}

    for index, event in enumerate(events):
        if event.src in previous:
            deps[index].add(previous[event.src])
        previous[event.src] = index

        if event.kind == "RECV":
            recv_by.setdefault((event.src, event.obj), []).append(index)
            if event.payload:
                recvs.setdefault(event.payload, []).append(index)
        elif event.kind in ("REPLY", "ERROR"):
            for recv_index in recv_by.get((event.src, event.obj), []):
                deps[index].add(recv_index)
        elif event.kind == "SEND" and event.payload:
            sends.setdefault(event.payload, []).append(index)

    # Send -> receive: the k-th send with a unique payload precedes the k-th
    # receive of it. Conformance payloads carry a unique token, so the pairing
    # is unambiguous; duplicate payloads pair in order.
    for payload, send_indices in sends.items():
        for send_index, recv_index in zip(send_indices,
                                          recvs.get(payload, [])):
            deps[recv_index].add(send_index)

    return events, deps


def canonical_extension(events, deps) -> list[int]:
    """Kahn's algorithm with a content-ordered heap: one canonical order."""
    indegree = [0] * len(events)
    children: list[list[int]] = [[] for _ in events]
    for index, parents in enumerate(deps):
        indegree[index] = len(parents)
        for parent in parents:
            children[parent].append(index)

    heap = [(events[index].key(), index) for index in range(len(events))
            if indegree[index] == 0]
    heapq.heapify(heap)
    order = []
    while heap:
        _, index = heapq.heappop(heap)
        order.append(index)
        for child in children[index]:
            indegree[child] -= 1
            if indegree[child] == 0:
                heapq.heappush(heap, (events[child].key(), child))

    if len(order) != len(events):
        raise ValueError("causal graph has a cycle")
    return order


@dataclass
class Result:
    equal: bool
    detail: str
    left: list[Event] = field(default_factory=list)
    right: list[Event] = field(default_factory=list)
    divergence: int | None = None

    def as_dict(self) -> dict:
        payload = {"equal": self.equal, "detail": self.detail}
        if self.divergence is not None:
            payload["divergence"] = self.divergence
        return payload


def _edge_set(events, deps, order) -> set[tuple[tuple, tuple]]:
    del order  # edges are already key-based, independent of the extension
    edges = set()
    for index, parents in enumerate(deps):
        for parent in parents:
            edges.add((events[parent].key(), events[index].key()))
    return edges


def compare_events(left_events, left_deps, right_events, right_deps,
                   strict_time: bool = False) -> Result:
    left_order = canonical_extension(left_events, left_deps)
    right_order = canonical_extension(right_events, right_deps)

    left_keys = [(left_events[i].key(),
                  left_events[i].t_ns if strict_time else 0)
                 for i in left_order]
    right_keys = [(right_events[i].key(),
                   right_events[i].t_ns if strict_time else 0)
                  for i in right_order]

    for position, (a, b) in enumerate(zip(left_keys, right_keys)):
        if a != b:
            return Result(False, f"event {position} differs", divergence=position)
    if len(left_keys) != len(right_keys):
        position = min(len(left_keys), len(right_keys))
        return Result(False, f"event count differs at {position}",
                      divergence=position)

    left_edges = _edge_set(left_events, left_deps, left_order)
    right_edges = _edge_set(right_events, right_deps, right_order)
    if left_edges != right_edges:
        missing = left_edges - right_edges
        extra = right_edges - left_edges
        return Result(False,
                      f"causal edges differ ({len(missing)} missing, "
                      f"{len(extra)} extra)")

    return Result(True, f"{len(left_keys)} events equivalent")


def compare_records(left_records, right_records,
                    strict_time: bool = False) -> Result:
    left_events, left_deps = canonicalise(left_records)
    right_events, right_deps = canonicalise(right_records)
    return compare_events(left_events, left_deps, right_events, right_deps,
                          strict_time=strict_time)


# ---------------------------------------------------------------------------
# Files


def load_records(path: Path):
    path = Path(path)
    if path.suffix == ".jsonl":
        return [json.loads(line) for line in path.read_text().splitlines()
                if line.strip()]
    from cli.permute.trace import codec
    return codec.read(path).records


def load_canonical(path: Path):
    lines = [json.loads(line) for line in Path(path).read_text().splitlines()
             if line.strip()]
    events = []
    deps = []
    for line in lines:
        events.append(Event(
            kind=line["kind"], src=line["src"], dst=line.get("dst"),
            obj=line.get("obj"), aux=line.get("aux"),
            payload=line.get("payload"), t_ns=line.get("t_ns") or 0,
        ))
        deps.append(set(line.get("causal", [])))
    return events, deps


def to_jsonl(records) -> str:
    events, deps = canonicalise(records)
    order = canonical_extension(events, deps)
    slot = {index: position for position, index in enumerate(order)}
    lines = []
    for index in order:
        payload = events[index].as_dict(
            [slot[parent] for parent in deps[index]])
        payload["schema"] = SCHEMA
        lines.append(json.dumps(payload, sort_keys=True,
                                separators=(",", ":")))
    return "\n".join(lines) + "\n"


def main(argv=None) -> int:
    import argparse

    parser = argparse.ArgumentParser(prog="tracecmp")
    parser.add_argument("left")
    parser.add_argument("right")
    parser.add_argument("--strict-time", action="store_true")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    result = compare_records(load_records(Path(args.left)),
                             load_records(Path(args.right)),
                             strict_time=args.strict_time)
    if args.json:
        print(json.dumps(result.as_dict(), sort_keys=True))
    else:
        print(f"{'equivalent' if result.equal else 'different'}: {result.detail}")
    return 0 if result.equal else 1


if __name__ == "__main__":
    sys.exit(main())
