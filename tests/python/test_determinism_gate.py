# SPDX-License-Identifier: Apache-2.0
"""Determinism gate: positive and negative cases.

The gate is only worth having if it fails on a planted violation, so each
category gets a fixture that must be caught, plus one that must not be (a
comment, and the allowlisted routing shim).
"""

import importlib.util
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("symcheck",
                                              REPO / "tools" / "symcheck.py")
symcheck = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(symcheck)

checks = 0
failures = 0


def check(cond, message):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL {message}")


_KEEP: list = []


def tree(files: dict[str, str]) -> Path:
    tmp = tempfile.TemporaryDirectory()
    _KEEP.append(tmp)  # keep the directory alive for the process
    root = Path(tmp.name)
    for rel, text in files.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    return root


def violations(files: dict[str, str]) -> list[str]:
    return symcheck.scan_tree(tree(files))


def main():
    check(violations({"kernel/ok.c": "int f(void) { return 1; }\n"}) == [],
          "clean tree should pass")

    check(any("time:" in v for v in violations({
        "kernel/bad.c": "void f(void){ struct timespec t; "
                        "clock_gettime(CLOCK_REALTIME, &t); }\n"})),
          "real clock must be caught")

    check(any("entropy:" in v for v in violations({
        "kernel/bad.c": "int f(void){ return rand(); }\n"})),
          "rand must be caught")

    check(any("container:" in v for v in violations({
        "kernel/bad.c": "void f(void){ hcreate(16); }\n"})),
          "hash container must be caught")

    check(any("identity:" in v for v in violations({
        "guest/bad.c": "int f(void){ return (int)getpid(); }\n"})),
          "host pid must be caught")

    # A mention in a comment is documentation, not a call.
    check(violations({
        "kernel/doc.c": "/* clock_gettime(CLOCK_REALTIME) is banned here */\n"
                        "int f(void){ return 0; }\n"}) == [],
          "comment must not trip the gate")

    # The routing shims are the explicit exception.
    check(violations({
        "abi/shim_time.c": "int clock_gettime(int a, void *b)"
                           "{ (void)a; (void)b; return 0; }\n",
        "abi/shim_proc.c": "int getpid(void){ return -1; }\n",
    }) == [],
          "allowlisted shims must pass")

    # The real tree must be clean, and the real archive must define its symbols.
    result = subprocess.run([sys.executable, str(REPO / "tools" / "symcheck.py")],
                            capture_output=True, text=True, cwd=REPO)
    check(result.returncode == 0,
          f"repo gate failed:\n{result.stdout}\n{result.stderr}")
    check("determinism gate: PASSED" in result.stdout,
          "repo gate should print a pass line")

    print(f"test_determinism_gate: {checks} checks, {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
