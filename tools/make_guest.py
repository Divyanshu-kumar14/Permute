#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build one guest process instance as a shared object.

Each simulated process is its own .so (ADR-005). The version script is derived
from abi/permute_iface.def, so the exported ABI cannot drift from the single
source of truth, and `nm` is checked to prove the interposed symbols are
defined in the object rather than imported from libc.

    make_guest.py --source S --name N --instance I --out DIR
    make_guest.py --check-linker-script
    make_guest.py --write-linker-script
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEF = REPO / "abi" / "permute_iface.def"
SCRIPT = REPO / "guest" / "permute_guest.ld"

SYMBOL_RE = re.compile(r"PERMUTE_SYMBOL\(\s*[^,]+,\s*[^,]+,\s*(\w+)")


def interposed_symbols() -> list[str]:
    # Strip line comments first: the file documents the macro form in a comment
    # that would otherwise parse as a symbol named "name".
    raw = DEF.read_text()
    text = "\n".join(line.split("//", 1)[0] for line in raw.splitlines())
    seen = []
    for name in SYMBOL_RE.findall(text):
        if name not in seen:
            seen.append(name)
    if not seen:
        raise SystemExit(f"no symbols parsed from {DEF}")
    return seen


def render_script(symbols: list[str]) -> str:
    lines = [
        "/* SPDX-License-Identifier: Apache-2.0",
        " *",
        " * Version script for a guest process object.",
        " *",
        " * Exports `permute_main` and the interposed ABI, and hides everything else.",
        " * The interposed list is generated from abi/permute_iface.def; run",
        " * `tools/make_guest.py --check-linker-script` to detect drift.",
        " */",
        "{",
        "  global:",
        "    permute_main;",
        "    permute_hypercall_bind;",
    ]
    lines += [f"    {name};" for name in symbols]
    lines += ["  local:", "    *;", "};", ""]
    return "\n".join(lines)


def script_symbols(path: Path) -> list[str]:
    text = path.read_text()
    start = text.index("global:")
    end = text.index("local:")
    body = text[start + len("global:"):end]
    return [line.strip().rstrip(";") for line in body.splitlines() if line.strip()]


def check_script(symbols: list[str]) -> int:
    if not SCRIPT.is_file():
        print(f"error: {SCRIPT} is missing", file=sys.stderr)
        return 1
    have = set(script_symbols(SCRIPT))
    want = set(symbols) | {"permute_main", "permute_hypercall_bind"}
    if have != want:
        print("error: guest/permute_guest.ld is out of date", file=sys.stderr)
        for name in sorted(want - have):
            print(f"  missing: {name}", file=sys.stderr)
        for name in sorted(have - want):
            print(f"  extra:   {name}", file=sys.stderr)
        print("  run tools/make_guest.py --write-linker-script", file=sys.stderr)
        return 1
    return 0


def verify_object(path: Path, symbols: list[str]) -> int:
    nm = subprocess.run(["nm", "-D", "--defined-only", str(path)],
                        capture_output=True, text=True)
    if nm.returncode != 0:
        print(f"error: nm failed on {path}: {nm.stderr}", file=sys.stderr)
        return 1

    defined = set()
    for line in nm.stdout.splitlines():
        fields = line.split()
        if fields:
            defined.add(fields[-1].split("@")[0])

    if "permute_main" not in defined:
        print(f"error: {path.name} does not export permute_main", file=sys.stderr)
        return 1

    # Any interposed symbol the object uses must resolve locally. Requiring
    # *every* symbol would force a trivial SUT to reference calls it never
    # makes; --no-undefined already guarantees the used ones resolve, and this
    # check proves they did not resolve to libc.
    nm_u = subprocess.run(["nm", "-D", "--undefined-only", str(path)],
                          capture_output=True, text=True)
    imported = []
    for line in nm_u.stdout.splitlines():
        fields = line.split()
        if fields:
            name = fields[-1].split("@")[0]
            if name in symbols:
                imported.append(name)
    if imported:
        print(f"error: {path.name} imports interposed symbols from libc: "
              f"{', '.join(sorted(set(imported)))}", file=sys.stderr)
        return 1

    # The loader's internals must not leak into the guest's dynamic symbol
    # table; the version script is what guarantees it.
    leaked = sorted(n for n in defined
                    if n.startswith("permute_")
                    and n not in ("permute_main", "permute_hypercall_bind"))
    if leaked:
        print(f"error: {path.name} exports internals: {', '.join(leaked)}",
              file=sys.stderr)
        return 1
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source")
    ap.add_argument("--name")
    ap.add_argument("--instance")
    ap.add_argument("--out", default="build")
    ap.add_argument("--build-dir", default="build")
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
    ap.add_argument("--write-linker-script", action="store_true")
    ap.add_argument("--check-linker-script", action="store_true")
    args = ap.parse_args()

    symbols = interposed_symbols()

    if args.write_linker_script:
        SCRIPT.write_text(render_script(symbols))
        print(f"wrote {SCRIPT}")
        return 0

    if args.check_linker_script:
        return check_script(symbols)

    if not (args.source and args.name and args.instance is not None):
        print("error: --source, --name and --instance are required", file=sys.stderr)
        return 2

    if check_script(symbols) != 0:
        return 1

    build = Path(args.build_dir).resolve()
    archive = build / "libpermute_guest.a"
    if not archive.is_file():
        print(f"error: {archive} not found (run make build)", file=sys.stderr)
        return 1

    out = Path(args.out).resolve() / f"proc_{args.name}__{args.instance}.so"

    cmd = [
        args.cc, "-std=c11", "-O2", "-fPIC", "-shared",
        "-D_GNU_SOURCE",
        "-Wall", "-Wextra", "-Wpedantic",
        "-I", str(REPO),
        "-DGUEST_INSTANCE=\"" + str(args.instance) + "\"",
        "-o", str(out), str(Path(args.source).resolve()),
        f"-Wl,--version-script={SCRIPT}",
        # A shared object's own references are resolved in the global scope
        # first, so libc would win over the shim's clock, identity, and sleep
        # definitions. Binding intra-object calls to the object's own symbols
        # is what makes the shim authoritative inside a dlopen'd guest.
        "-Wl,-Bsymbolic-functions",
        # Every guest must be bindable, even one that calls nothing: the
        # loader resolves this symbol per object.
        "-Wl,-u,permute_hypercall_bind",
        "-Wl,--no-undefined",
        "-L", str(build), "-lpermute_guest",
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print("error: guest compile failed", file=sys.stderr)
        print(" ".join(cmd), file=sys.stderr)
        print(result.stdout, file=sys.stderr)
        print(result.stderr, file=sys.stderr)
        return 1

    if verify_object(out, symbols) != 0:
        return 1

    print(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
