# SPDX-License-Identifier: Apache-2.0
"""P2-05: the canonicaliser and the Mazurkiewicz comparator.

The scenarios themselves are exercised by `make conformance`; these checks pin
the properties the comparison relies on: independent events may move, causal
events may not, payloads and errnos matter, and time is excluded by default.
"""

from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from conformance.tracecmp import compare_records  # noqa: E402

checks = 0
failures = 0


def check(cond, message):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL {message}")


def rec(kind, seq, src=1, dst=0, obj=0, aux=0, blob=None, t_ns=0):
    return {"kind": kind, "seq": seq, "src": src, "dst": dst, "obj": obj,
            "aux": aux, "blob": blob, "t_ns": t_ns}


def base():
    return [
        rec("THREAD_START", 1, src=1, obj=10),
        rec("THREAD_START", 2, src=2, obj=10),
        rec("CHANNEL_CREATE", 3, src=1, obj=0),
        rec("CHANNEL_CREATE", 4, src=2, obj=1),
    ]


def test_identical():
    result = compare_records(base(), base())
    check(result.equal, f"identical: {result.detail}")


def test_independent_reorder():
    # Swap the two channel creations. They are independent (different threads,
    # no shared object), so the canonical extension must be the same.
    a = base()
    b = base()
    b[2], b[3] = b[3], b[2]
    check(compare_records(a, b).equal,
          "independent: reordered events should be equivalent")


def test_payload_matters():
    a = [
        rec("THREAD_START", 1, src=1),
        rec("THREAD_START", 2, src=2),
        rec("SEND", 3, src=1, obj=0, blob=b"A"),
        rec("RECV", 4, src=2, obj=7, blob=b"A"),
        rec("REPLY", 5, src=2, obj=7),
    ]
    b = [dict(record) for record in a]
    b[3]["blob"] = b"B"
    check(not compare_records(a, b).equal,
          "payload: a different byte must be a difference")


def test_causal_order_matters():
    a = [
        rec("THREAD_START", 1, src=1),
        rec("THREAD_START", 2, src=2),
        rec("SEND", 3, src=1, obj=0, blob=b"A"),
        rec("RECV", 4, src=2, obj=7, blob=b"A"),
        rec("REPLY", 5, src=2, obj=7),
    ]
    # A reply cannot exist before the receive that produced its rcvid. Moving
    # it loses the receive -> reply edge, so the partial orders differ.
    b = [a[0], a[1], a[4], a[2], a[3]]
    check(not compare_records(a, b).equal,
          "causal: reply before receive must not be equivalent")

    # A receive written before its send is still the same partial order: the
    # payload match infers the edge and the extension reorders them.
    c = [a[0], a[1], a[3], a[2], a[4]]
    check(compare_records(a, c).equal,
          "causal: the payload edge is inferred regardless of record order")


def test_event_count():
    a = base()
    b = base() + [rec("CHANNEL_CREATE", 5, src=1, obj=2)]
    check(not compare_records(a, b).equal, "count: an extra event differs")


def test_time_is_ignored_by_default():
    a = base()
    b = base()
    for index, record in enumerate(b):
        record["t_ns"] = index * 1000
    check(compare_records(a, b).equal,
          "time: timestamps are excluded by default")
    check(not compare_records(a, b, strict_time=True).equal,
          "time: --strict-time includes them")


def test_errno_matters():
    a = [
        rec("THREAD_START", 1, src=1),
        rec("THREAD_START", 2, src=2),
        rec("RECV", 3, src=2, obj=7),
        rec("REPLY", 4, src=2, obj=7, aux=0),
    ]
    b = [dict(record) for record in a]
    b[3]["aux"] = 3  # ESRCH
    check(not compare_records(a, b).equal,
          "errno: a reply verdict must be compared")


def main() -> int:
    test_identical()
    test_independent_reorder()
    test_payload_matters()
    test_causal_order_matters()
    test_event_count()
    test_time_is_ignored_by_default()
    test_errno_matters()
    print(f"test_conformance: {checks} checks, {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
