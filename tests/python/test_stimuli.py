# SPDX-License-Identifier: Apache-2.0
"""_stimuli time parsing: no silent coercion to zero.

A stimulus `at=` value such as "1.5s" must parse to its real nanosecond time,
and anything unparseable must raise ConfigError instead of collapsing to 0,
which would hide a user error inside an explore tape.
"""

from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from cli.permute import config  # noqa: E402
from cli.permute.__main__ import _stimuli  # noqa: E402

checks = 0
failures = 0


def check(cond, message):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL {message}")


def expect(text, want):
    check(config.parse_stimulus_time(text) == want,
          f"parse_stimulus_time({text!r}) == {want}")


def expect_fail(text):
    try:
        config.parse_stimulus_time(text)
    except config.ConfigError:
        return
    check(False, f"parse_stimulus_time({text!r}) should raise ConfigError")


def main():
    expect("1.5s", 1_500_000_000)
    expect("250ms", 250_000_000)
    expect("1000us", 1_000_000)
    expect("50000ns", 50_000)
    expect("1500", 1500)  # bare number = nanoseconds
    expect("0s", 0)
    expect("0", 0)

    expect_fail("abc")
    expect_fail("s")
    expect_fail("-5s")
    expect_fail("")
    expect_fail("nan")
    expect_fail("inf")
    expect_fail("1e999")

    # config.validate rejects a bad time for every command, not just --explore,
    # and leaves a valid `at` untouched for the artifact writer.
    for bad in ("garbage", "nan", "-5s"):
        try:
            config.validate({"project": {"name": "t"},
                             "stimulus": [{"at": bad, "kind": "pulse"}]})
        except config.ConfigError:
            pass
        else:
            check(False, f"config.validate accepts stimulus at={bad!r}")
    data = config.validate({"project": {"name": "t"},
                            "stimulus": [{"at": "1.5s", "kind": "pulse"}]})
    check(data["stimulus"][0]["at"] == "1.5s", "config.validate keeps at")

    # _stimuli propagates parsed times into at_ns and preserves other keys.
    out = _stimuli([{"at": "1.5s", "kind": "pulse"},
                    {"at": "250ms", "kind": "send"}])
    check(out[0]["at_ns"] == 1_500_000_000, "_stimuli 1.5s")
    check(out[1]["at_ns"] == 250_000_000, "_stimuli 250ms")
    check(out[0]["kind"] == "pulse", "_stimuli preserves other keys")

    if failures:
        print(f"{failures}/{checks} check(s) failed")
        raise SystemExit(1)
    print(f"{checks} check(s), 0 failure(s)")


if __name__ == "__main__":
    main()
