<!-- SPDX-License-Identifier: Apache-2.0 -->

# P4-03 report — clean-machine dry run

**Date:** 2026-11-14
**Scope:** NFR-9, NFR-10, §13.4 — the full flow from a source tarball in a
fresh network namespace, with no SDP installed, in ≤ 15 min.
**Result:** **PASS.** `tools/clean_room.sh` completed in **195.5 s** (3 min
16 s) inside `unshare -rn`, with outbound TCP and DNS proven impossible,
39 files installed, the installed CLI exercised, and zero uninstall residue.

---

## 1. Procedure

```sh
tools/clean_room.sh
```

The script:

1. refuses a dirty tree and exports `git archive HEAD` (prefix `permute/`), so
   the evidence names a commit and no local build output can leak in;
2. extracts into an empty directory;
3. asserts that a TCP connect to `1.1.1.1:80` and a DNS/TCP attempt to
   `example.com:80` both fail — a machine that still has network access cannot
   produce a false pass;
4. runs `make build`, `make ci`, `make examples`, `make install`,
   `permute --version`, installed `init`/`build`/`run`, and `make uninstall`;
5. fails if the prefix survives uninstall or the total exceeds 900 s.

No QNX SDP is installed or referenced anywhere in the flow.

## 2. Measured

| Phase | Command | Wall |
|---|---|---|
| `build` | `make build` | **1,385 ms** |
| `ci` | `make ci` (unit, codec, replay, allocator, interposition, determinism 1,000 runs, coverage, docs, quickstart, examples, licence, CI-check) | **189,179 ms** |
| `examples` | `make examples` | **3,670 ms** |
| `install` | `make install PREFIX=<temp>` | **209 ms** |
| `version` | `<prefix>/bin/permute --version` | **72 ms** |
| `project` | installed `init` → `build` → `run` | **853 ms** |
| `uninstall` | `make uninstall PREFIX=<temp>` | **89 ms** |
| **total** | | **195,457 ms** |

```text
offline:  no outbound TCP connection is possible
routes:   0 route line(s)
installed files: 39; uninstall residue: 0
version: permute 0.1.0 (git 3417926, kernel model 1)
clean-room: PASSED (195457 ms inside the namespace, 195612 ms wall, limit 900 s)
```

The 15-minute budget is met with 12 min to spare. The long pole is `make ci`,
which is the intended merge gate; a reader in a hurry can run `make build`,
`make test` and the installed smoke in well under a minute.

## 3. What the first run caught

The initial script failed after the uninstall phase: `residue=$(find "$prefix"
… | wc -l)` returns non-zero when the prefix no longer exists, and
`set -euo pipefail` turned that into an abort. The fix records the residue with
a guarded `find` and makes the outer script print the partial phase table on
failure instead of losing it. This is why the gate asserts residue rather than
printing it: the first version could not distinguish "zero residue" from "the
check crashed".

## 4. Scope

Proven: the flow needs no network, no QNX licence, and no state outside the
tarball; installation is complete enough to build and run a project; uninstall
removes exactly what install created.

Not proven: a different CPU, distro, libc or architecture (the nightly CI
matrix covers musl and aarch64, NFR-11); a cold package cache on a machine
without a compiler (the flow uses the host toolchain documented in
[`docs/REPRODUCE.md`](../../docs/REPRODUCE.md) §1); and target fidelity, which
needs the SDP (section 5 of the same document).

## 5. Reproduce

```sh
tools/clean_room.sh
# or, from an existing artifact:
tools/clean_room.sh dist/permute-0.1.0.tar.gz
```

Logs and the raw phase table are left in the printed work directory
(`/tmp/opencode/permute-clean-<timestamp>` by default,
`PERMUTE_CLEAN_DIR` to choose).
