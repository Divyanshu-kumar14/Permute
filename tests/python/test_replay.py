# SPDX-License-Identifier: Apache-2.0
"""Tests for replay: seed determinism, artifact verification, exit codes."""

import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from cli.permute.run import (  # noqa: E402
    EXIT_DIGEST_MISMATCH, EXIT_OK, EXIT_REFUSED, EXIT_USAGE, Manifest,
    replay_artifact, replay_seed, verify_ordering,
)
from cli.permute.trace import codec  # noqa: E402

FIXTURE = REPO / "build" / "replay_fixture"

checks = 0
failures = 0


def check(cond, message):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL {message}")


def make_trace(tmp, seed=42, name="seed.pmt"):
    path = Path(tmp) / name
    out = subprocess.run([str(FIXTURE), str(seed), str(path)],
                         capture_output=True, text=True, check=True)
    fields = {}
    for line in out.stdout.splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            fields[key] = value
    return path, fields


def test_seed_determinism():
    a = replay_seed(42, FIXTURE, runs=2)
    b = replay_seed(43, FIXTURE, runs=2)

    check(a.exit_code == EXIT_OK, f"seed 42 replay: {a.messages}")
    check(b.exit_code == EXIT_OK, f"seed 43 replay: {b.messages}")
    check(a.digest is not None and b.digest is not None, "digests present")
    check(a.digest != b.digest, "different seeds must differ")
    check(a.records > 100, f"records {a.records}")


def test_artifact_ok(tmp):
    path, fields = make_trace(tmp)
    tr = codec.read(path)

    check(tr.created_unix_ns == 0, "artifact must carry no wall-clock time")
    check(tr.kernel_model_version == int(fields["KERNEL"]),
          "kernel model version recorded")

    result = replay_artifact(path)
    check(result.exit_code == EXIT_OK, f"plain replay: {result.messages}")
    check(result.digest == int(fields["DIGEST"], 16), "digest matches writer")

    manifest = Manifest(
        build_id=bytes.fromhex(fields["BUILD"]),
        config_hash=int(fields["CONFIG"], 16),
        kernel_model_version=int(fields["KERNEL"]),
        run_digest=int(fields["DIGEST"], 16),
    )
    result = replay_artifact(path, manifest)
    check(result.exit_code == EXIT_OK, f"manifest replay: {result.messages}")
    return path, fields


def test_manifest_refusal(tmp, path, fields):
    good_build = bytes.fromhex(fields["BUILD"])
    good_config = int(fields["CONFIG"], 16)
    good_kernel = int(fields["KERNEL"])

    bad = bytearray(good_build)
    bad[0] ^= 0xFF

    r = replay_artifact(path, Manifest(build_id=bytes(bad)))
    check(r.exit_code == EXIT_REFUSED, f"build mismatch exit {r.exit_code}")
    check(any("build id" in m for m in r.messages), "build mismatch named")

    r = replay_artifact(path, Manifest(config_hash=good_config ^ 1))
    check(r.exit_code == EXIT_REFUSED, f"config mismatch exit {r.exit_code}")

    r = replay_artifact(path, Manifest(kernel_model_version=good_kernel + 1))
    check(r.exit_code == EXIT_REFUSED, f"kernel mismatch exit {r.exit_code}")

    # --force accepts a manifest mismatch.
    r = replay_artifact(path, Manifest(build_id=bytes(bad)), force=True)
    check(r.exit_code == EXIT_OK, f"forced replay exit {r.exit_code}")
    check(any("forced past" in m for m in r.messages), "force is recorded")

    # It does not accept a digest mismatch: that check is authoritative.
    r = replay_artifact(path, Manifest(run_digest=1), force=True)
    check(r.exit_code == EXIT_DIGEST_MISMATCH,
          f"digest mismatch exit {r.exit_code}")


def test_corruption_refused(tmp, path):
    raw = bytearray(path.read_bytes())
    raw[codec.HEADER_SIZE + 100] ^= 0xFF
    bad = Path(tmp) / "corrupt.pmt"
    bad.write_bytes(raw)

    r = replay_artifact(bad)
    check(r.exit_code == EXIT_USAGE, f"corrupt exit {r.exit_code}")
    check(any("unusable" in m for m in r.messages), "corrupt is named")

    cut = Path(tmp) / "partial.pmt"
    cut.write_bytes(path.read_bytes()[:-100])
    r = replay_artifact(cut)
    check(r.exit_code == EXIT_USAGE, f"partial exit {r.exit_code}")
    check(any("incomplete" in m for m in r.messages), "partial is named")


def test_ordering_check(tmp):
    path, _ = make_trace(tmp, name="ordering.pmt")
    tr = codec.read(path)
    check(verify_ordering(tr.records) == [], "reader output is ordered")

    broken = [dict(tr.records[0]), dict(tr.records[1])]
    broken[1]["seq"] = broken[0]["seq"]          # not increasing
    broken[1]["t_ns"] = broken[0]["t_ns"] - 1    # time went backwards
    problems = verify_ordering(broken)
    check(any("seq" in p for p in problems), f"seq regression: {problems}")
    check(any("backwards" in p for p in problems), "time regression caught")

    missing = [dict(tr.records[0])]
    for key in ("obj", "aux", "dst"):
        missing[0][key] = None
    problems = verify_ordering(missing)
    check(len(problems) >= 1, "missing schema field caught")


def test_cli(tmp, path, fields):
    env = {"PYTHONPATH": str(REPO)}
    base = [sys.executable, "-m", "cli.permute.run"]

    out = subprocess.run(base + ["seed", "42", "--fixture", str(FIXTURE),
                                 "--json"],
                         capture_output=True, text=True, cwd=REPO, env=env)
    check(out.returncode == EXIT_OK, f"cli seed exit {out.returncode}: {out.stderr}")
    payload = json.loads(out.stdout)
    check(payload["ok"] is True, "cli seed json ok")

    bad = bytes.fromhex(fields["BUILD"])
    bad = bytes([bad[0] ^ 0xFF]) + bad[1:]
    out = subprocess.run(base + ["artifact", str(path), "--build-id", bad.hex()],
                         capture_output=True, text=True, cwd=REPO, env=env)
    check(out.returncode == EXIT_REFUSED,
          f"cli mismatch exit {out.returncode}: {out.stdout}{out.stderr}")

    out = subprocess.run(base + ["artifact", str(path), "--build-id", bad.hex(),
                                 "--force"],
                         capture_output=True, text=True, cwd=REPO, env=env)
    check(out.returncode == EXIT_OK, f"cli force exit {out.returncode}")


def main():
    if not FIXTURE.exists():
        raise SystemExit(f"missing {FIXTURE}; run make build")

    test_seed_determinism()

    with tempfile.TemporaryDirectory() as tmp:
        path, fields = test_artifact_ok(tmp)
        test_manifest_refusal(tmp, path, fields)
        test_corruption_refused(tmp, path)
        test_ordering_check(tmp)
        test_cli(tmp, path, fields)

    print(f"test_replay: {checks} checks, {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
