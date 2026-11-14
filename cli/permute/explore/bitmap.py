# SPDX-License-Identifier: Apache-2.0
"""Novelty bitmap over decision-state and causal-edge features (FR-5.4).

A feature is a short string derived from the trace: which thread was picked
after which, which reply answered which receive, which fault fired. Hashing it
into a fixed bitmap makes coverage countable and keeps the corpus bounded.
"""

from __future__ import annotations

from ..trace import blake3

BITS = 1 << 16


class NoveltyBitmap:
    def __init__(self, bits: int = BITS):
        self.bits = bits
        self.words = bytearray(bits // 8)
        self.set_bits = 0

    def add(self, features) -> int:
        """Set each feature's bit and return how many were new."""
        new = 0
        for feature in features:
            index = blake3.hash64(feature.encode()) % self.bits
            byte, mask = divmod(index, 8)
            bit = 1 << mask
            if not self.words[byte] & bit:
                self.words[byte] |= bit
                self.set_bits += 1
                new += 1
        return new

    @property
    def coverage(self) -> int:
        return self.set_bits


def features(records) -> list[str]:
    """Decision-state and causal-edge features of one trace."""
    found: list[str] = []
    prev_pick = None
    rcvid_reply = {}
    payload_recv = {}
    pending_send = []

    for record in records:
        kind = record["kind"]
        src = record.get("src") or 0
        obj = record.get("obj")
        blob = record.get("blob")

        if kind == "SCHED_PICK":
            found.append(f"pick:{prev_pick}->{src}")
            prev_pick = src
        elif kind == "BLOCK":
            found.append(f"block:{record.get('aux') or 0}")
        elif kind == "RECV":
            if obj:
                rcvid_reply[(src, obj)] = True
            if blob:
                payload_recv.setdefault(blob, 0)
                payload_recv[blob] += 1
                found.append(f"recv:{blob.hex()[:12]}")
        elif kind == "REPLY":
            if rcvid_reply.pop((src, obj), False):
                found.append(f"reply:{obj}")
        elif kind == "SEND" and blob:
            pending_send.append(blob)
        elif kind == "FAULT_INJECT":
            found.append(f"fault:{obj}:{record.get('aux')}")

    # A send whose payload was received is a causal edge, not just an event.
    for blob in pending_send:
        if blob in payload_recv:
            found.append(f"edge:{blob.hex()[:12]}")
    return found
