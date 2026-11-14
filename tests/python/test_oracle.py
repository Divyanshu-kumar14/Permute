# SPDX-License-Identifier: Apache-2.0
"""P2-02: one positive and one negative fixture per oracle.

A fixture is a record stream, because an oracle is a post-hoc predicate over
the log (FR-10.1). Fixtures that need kernel behaviour the simulator does not
model yet — mutex inheritance is the only one — say so in their stream rather
than pretending a scenario exists.

The deadlock fixture also pins the rendered cycle, which is the string the
product prints: `T1(REPLY 65537) -> T2(REPLY 65538) -> T1`.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from cli.permute import config  # noqa: E402
from cli.permute.oracle import evaluate  # noqa: E402
from cli.permute.trace import codec  # noqa: E402

checks = 0
failures = 0


def check(cond, message):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL {message}")


def rec(kind, src=1, dst=0, obj=0, aux=0, blob=None, t_ns=None):
    return {"kind": kind, "src": src, "dst": dst, "obj": obj, "aux": aux,
            "blob": blob, "t_ns": t_ns}


def trace(*records):
    out = []
    seq = 0
    now = 0
    for record in records:
        seq += 1
        now = record["t_ns"] if record["t_ns"] is not None else now + 1000
        out.append({**record, "seq": seq, "t_ns": now})
    return out


def u32(value):
    return int(value).to_bytes(4, "little")


def invariant(kind, severity="error", **params):
    return {"id": f"{kind}-t", "kind": kind, "severity": severity, **params}


def only(records, rule):
    return evaluate(records, [rule]).findings


def classify(findings, kind):
    return [f for f in findings if f.kind == kind]


START = lambda tid, prio=10, name=b"A": rec("THREAD_START", src=tid, obj=prio,
                                            blob=name)
BLOCK = lambda tid, state, obj=0, peer=0, t=None: rec(
    "BLOCK", src=tid, obj=obj, aux=state, blob=u32(peer) if peer else None,
    t_ns=t)
UNBLOCK = lambda tid, obj=0, state=0, t=None: rec(
    "UNBLOCK", src=tid, obj=obj, aux=state, t_ns=t)
PICK = lambda tid, prio: rec("SCHED_PICK", src=tid, obj=prio)

SEND, RECEIVE, REPLY, MUTEX, JOIN = 3, 5, 4, 7, 10


# ---------------------------------------------------------------------------
# Fixtures

def fixture_deadlock_positive():
    records = trace(
        START(1), START(2, name=b"B"),
        BLOCK(1, REPLY, obj=0x10001, peer=2),
        BLOCK(2, REPLY, obj=0x10002, peer=1),
    )
    findings = classify(only(records, invariant("deadlock")), "deadlock")
    check(len(findings) == 1, "deadlock positive: fires once")
    if findings:
        check(findings[0].detail ==
              "T1(REPLY 65537) -> T2(REPLY 65538) -> T1",
              f"deadlock positive: cycle {findings[0].detail!r}")


def fixture_deadlock_negative():
    records = trace(
        START(1), START(2, name=b"B"),
        BLOCK(1, REPLY, obj=0x10001, peer=2),
        BLOCK(2, REPLY, obj=0x10002, peer=1),
        UNBLOCK(2),
    )
    check(not classify(only(records, invariant("deadlock")), "deadlock"),
          "deadlock negative: no fire when one thread is released")


def fixture_unbounded_block_positive():
    records = trace(
        START(1, name=b"guest"),
        BLOCK(1, SEND, t=0),
        UNBLOCK(1, t=300_000_000),
    )
    findings = classify(
        only(records, invariant("unbounded_block", task="guest",
                                max_virtual_ms=250)), "unbounded_block")
    check(len(findings) == 1, "unbounded_block positive: fires")


def fixture_unbounded_block_negative():
    records = trace(
        START(1, name=b"guest"),
        BLOCK(1, SEND, t=0),
        UNBLOCK(1, t=100_000_000),
    )
    check(not classify(
        only(records, invariant("unbounded_block", task="guest",
                                max_virtual_ms=250)), "unbounded_block"),
        "unbounded_block negative: a short block does not fire")


def _inversion_records(inherit: int, owner_prio: int):
    return trace(
        START(1, prio=10), START(2, prio=20, name=b"B"),
        rec("BLOCK", src=2, obj=42, aux=MUTEX,
            blob=u32(1) + u32(20) + u32(inherit), t_ns=0),
        rec("SCHED_PICK", src=1, obj=owner_prio, t_ns=1000),
        UNBLOCK(2, t=5000),
    )


def fixture_priority_inversion_positive():
    findings = only(_inversion_records(inherit=1, owner_prio=10),
                    invariant("priority_inversion", max_virtual_us=2))
    applied = classify(findings, "priority_inversion")
    check(len(applied) == 1, "priority_inversion positive: fires")
    if applied:
        check(applied[0].classification == "pi_not_applied",
              "priority_inversion positive: classified pi_not_applied")
    # FR-10.4: the same inversion with inheritance unconfigured is a different
    # finding, and is informational rather than a kernel defect.
    other = classify(only(_inversion_records(inherit=0, owner_prio=10),
                          invariant("priority_inversion", max_virtual_us=2)),
                     "priority_inversion")
    check(len(other) == 1 and
          other[0].classification == "no_inheritance_configured" and
          other[0].severity == "info",
          "priority_inversion: no-inheritance case is distinguished")


def fixture_priority_inversion_negative():
    findings = only(_inversion_records(inherit=1, owner_prio=20),
                    invariant("priority_inversion", max_virtual_us=2))
    check(not classify(findings, "priority_inversion"),
          "priority_inversion negative: inherited priority silences it")


def _starvation_records(high_ready: bool):
    records = [START(1), START(2, name=b"B")]
    if high_ready:
        # A higher-priority thread is ready at every dispatch decision, so
        # waiting is scheduling, not starvation (FR-10.5).
        records.append(START(3, prio=20, name=b"C"))
    for _ in range(6):
        if high_ready:
            records.append(PICK(3, 20))
            records.append(BLOCK(3, REPLY, obj=1, peer=2))
            records.append(UNBLOCK(3))
        else:
            records.append(PICK(1, 10))
            records.append(BLOCK(1, REPLY, obj=1, peer=2))
            records.append(UNBLOCK(1))
    return trace(*records)


def fixture_starvation_positive():
    findings = classify(only(_starvation_records(high_ready=False),
                             invariant("starvation",
                                       max_ready_gap_events=3)), "starvation")
    check(len(findings) == 1, "starvation positive: fires")


def fixture_starvation_negative():
    findings = only(_starvation_records(high_ready=True),
                    invariant("starvation", max_ready_gap_events=3))
    check(not classify(findings, "starvation"),
          "starvation negative: higher-priority ready silences it")


def fixture_orphan_reply_positive():
    records = trace(
        rec("REPLY", src=1, obj=0x20001),
    )
    findings = classify(only(records, invariant("orphan_reply")),
                        "orphan_reply")
    check(len(findings) == 1, "orphan_reply positive: fires")


def fixture_orphan_reply_negative():
    records = trace(
        rec("RECV", src=1, obj=0x20001),
        rec("REPLY", src=1, obj=0x20001),
    )
    check(not classify(only(records, invariant("orphan_reply")),
                       "orphan_reply"),
          "orphan_reply negative: a matched reply is silent")


def fixture_lost_pulse_positive():
    records = trace(
        rec("PULSE_SEND", src=1, obj=3, aux=0x42),
        rec("PULSE_RECV", src=1, obj=0, aux=1),
    )
    findings = classify(only(records, invariant("lost_pulse")), "lost_pulse")
    check(len(findings) == 1, "lost_pulse positive: fires")


def fixture_lost_pulse_negative():
    records = trace(
        rec("PULSE_SEND", src=1, obj=3, aux=0x42),
        rec("PULSE_RECV", src=1, obj=0x42, aux=0),
    )
    check(not classify(only(records, invariant("lost_pulse")), "lost_pulse"),
          "lost_pulse negative: a delivered pulse is silent")


def fixture_queue_depth_positive():
    records = trace(
        START(1), START(2, name=b"B"), START(3, name=b"C"),
        BLOCK(1, SEND, obj=7),
        BLOCK(2, SEND, obj=7),
        BLOCK(3, SEND, obj=7),
    )
    findings = classify(
        only(records, invariant("queue_depth", channel=7, max_senders=2)),
        "queue_depth")
    check(len(findings) == 1, "queue_depth positive: fires on the third sender")


def fixture_queue_depth_negative():
    records = trace(
        START(1), START(2, name=b"B"),
        BLOCK(1, SEND, obj=7),
        BLOCK(2, SEND, obj=7),
    )
    check(not classify(
        only(records, invariant("queue_depth", channel=7, max_senders=2)),
        "queue_depth"),
        "queue_depth negative: two senders are within the limit")


FIXTURES = [
    ("deadlock", fixture_deadlock_positive, fixture_deadlock_negative),
    ("unbounded_block", fixture_unbounded_block_positive,
     fixture_unbounded_block_negative),
    ("priority_inversion", fixture_priority_inversion_positive,
     fixture_priority_inversion_negative),
    ("starvation", fixture_starvation_positive, fixture_starvation_negative),
    ("orphan_reply", fixture_orphan_reply_positive,
     fixture_orphan_reply_negative),
    ("lost_pulse", fixture_lost_pulse_positive, fixture_lost_pulse_negative),
    ("queue_depth", fixture_queue_depth_positive, fixture_queue_depth_negative),
]


# ---------------------------------------------------------------------------
# The configuration the fixtures run under is the configuration the CLI
# accepts. Unknown keys, required parameters, and unknown kinds are rejected.

def test_config_schema():
    def rejects(rule):
        try:
            config.validate({"project": {"name": "x"}, "invariant": [rule]})
        except config.ConfigError:
            return True
        return False

    check(not rejects({"kind": "deadlock"}), "config: deadlock accepted")
    check(not rejects({"kind": "priority_inversion", "max_virtual_us": 1}),
          "config: priority_inversion accepted")
    check(not rejects({"kind": "starvation", "max_ready_gap_events": 1}),
          "config: starvation accepted")
    check(not rejects({"kind": "queue_depth", "max_senders": 1}),
          "config: queue_depth accepted")
    check(not rejects({"kind": "unbounded_block", "state": "SEND",
                       "max_virtual_ms": 1}),
          "config: unbounded_block accepted")
    check(rejects({"kind": "deadlock", "max_senders": 4}),
          "config: a parameter from another kind is rejected")
    check(rejects({"kind": "deadlock", "bogus": 1}),
          "config: an unknown key is rejected")
    check(rejects({"kind": "unbounded_block"}),
          "config: a missing required parameter is rejected")
    check(rejects({"kind": "nope"}), "config: an unknown kind is rejected")
    check(rejects({"kind": "starvation", "max_ready_gap_events": 0}),
          "config: a non-positive budget is rejected")
    check(rejects({"kind": "queue_depth", "channel": "db-conn",
                   "max_senders": 2}),
          "config: a channel name is rejected (the trace has numbers)")


# ---------------------------------------------------------------------------
# Real traces: the predicates must run on what the executor actually writes.

RUNNER = REPO / "build" / "permute-run"
SCENARIOS = REPO / "build" / "scenarios"


def run_guest(name: str, trace_path: Path, faults: str | None = None):
    env = {**os.environ,
           "PERMUTE_GUEST": str(SCENARIOS / f"proc_{name}__0.so")}
    if faults is not None:
        env["PERMUTE_FAULTS"] = faults
    result = subprocess.run([str(RUNNER), "1", str(trace_path)], env=env,
                            capture_output=True, text=True, timeout=180)
    fields = {}
    for line in result.stdout.splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            fields[key] = value
    fields["exit"] = result.returncode
    return fields


def test_real_traces(tmp: Path):
    if not RUNNER.is_file() or not (SCENARIOS / "proc_s01_hello__0.so").is_file():
        raise SystemExit("missing build/permute-run or scenarios; run make build")

    # A clean run produces no findings for any oracle.
    clean = tmp / "clean.pmt"
    fields = run_guest("s01_hello", clean)
    check(fields.get("OUTCOME") == "CLEAN", "real: s01 is clean")
    records = codec.read(clean).records
    rules = [invariant(kind, **params) for kind, params in (
        ("deadlock", {}),
        ("lost_pulse", {}),
        ("orphan_reply", {}),
        ("unbounded_block", {"max_virtual_ms": 1000, "state": "REPLY"}),
        ("starvation", {"max_ready_gap_events": 50}),
        ("queue_depth", {"max_senders": 8}),
    )]
    result = evaluate(records, rules, seed=1)
    check(not result.findings,
          f"real: clean trace has findings {[f.detail for f in result.findings]}")
    check(not result.skipped, "real: guest trace supports every oracle")

    # s04 loses a pulse through the short-buffer rule.
    drop = tmp / "drop.pmt"
    run_guest("s04_pulse_drop", drop)
    findings = evaluate(codec.read(drop).records,
                        [invariant("lost_pulse")], seed=1).findings
    check(len(findings) == 1, "real: s04 reports a lost pulse")

    # A duplicated reply is an orphan: the second reply has no live rcvid.
    dup = tmp / "dup.pmt"
    fields = run_guest("s01_hello", dup, faults="reply:duplicate@1")
    findings = evaluate(codec.read(dup).records,
                        [invariant("orphan_reply")], seed=1).findings
    check(len(findings) >= 1,
          f"real: duplicated reply reports an orphan (exit {fields['exit']})")

    # A sender that loses its message with no receiver leaves every live
    # thread blocked; the deadlock oracle must fire even without a cycle.
    dead = tmp / "dead.pmt"
    run_guest("s01_hello", dead, faults="send:drop@1")
    findings = evaluate(codec.read(dead).records,
                        [invariant("deadlock")], seed=1).findings
    check(len(findings) == 1, "real: a dropped message deadlocks")


def main() -> int:
    for kind, positive, negative in FIXTURES:
        positive()
        negative()

    test_config_schema()
    with tempfile.TemporaryDirectory() as tmp:
        test_real_traces(Path(tmp))

    print(f"test_oracle: {checks} checks, {failures} failure(s) "
          f"({len(FIXTURES)} oracle(s), 2 fixtures each)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
