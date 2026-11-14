<!-- SPDX-License-Identifier: Apache-2.0 -->

# P4-01 report — install, uninstall, version, dist

**Date:** 2026-11-14
**Scope:** §16.1 — `git clone && make && sudo make install` succeeds;
`permute --version` prints version, git SHA and kernel model; `make uninstall`
leaves no residue; `make dist` produces a checksummed source tarball.
**Result:** **PASS.** 39 files install into a staged prefix, the installed CLI
initialises, builds and runs a project, uninstall removes every file and the
prefix directory, and an extracted tarball builds and reports its SHA without a
`.git` directory.

---

## 1. What is installed

`install.sh` treats `<prefix>/lib/permute` as `PERMUTE_HOME`, which is the same
layout `cli/permute/__main__.py` already resolves:

| Path | Contents |
|---|---|
| `bin/permute` | launcher: sets `PERMUTE_HOME`, bakes `PERMUTE_GIT_SHA`, sets `PYTHONDONTWRITEBYTECODE=1` |
| `lib/permute/cli/permute/` | every CLI module (`find … -name '*.py'`) |
| `lib/permute/abi/` | `permute_iface.def` and the public headers |
| `lib/permute/guest/` | `permute_guest.ld` |
| `lib/permute/build/` | `permute-run`, `permute-worker`, `libpermute_guest.a` |
| `lib/permute/tools/` | `make_guest.py`, `symcheck.py`, `header_gap.py` |
| `lib/permute/doc/` | `LICENSE`, `NOTICE`, `README.md`, `QUICKSTART.md`, `CHANGELOG.md` |
| `lib/permute/install-manifest.txt` | one relative path per installed file |

`--prefix`, `--destdir`, `--dry-run` and `--uninstall` are supported. A prefix
must be absolute. `make install` passes `PREFIX` and `DESTDIR` through, so the
standard staging pair works:

```sh
make install DESTDIR=/tmp/pkg PREFIX=/usr/local
```

The generated launcher names the **final** location (`/usr/local/lib/permute`),
not the staging path, which is what `DESTDIR` semantics require.

---

## 2. Install evidence

```sh
$ ./install.sh --prefix /tmp/opencode/p401/prefix
installed permute 32d9151 to /tmp/opencode/p401/prefix
run: /tmp/opencode/p401/prefix/bin/permute --version
installed_files=39

$ /tmp/opencode/p401/prefix/bin/permute --version
permute 0.1.0 (git 32d9151, kernel model 1)

$ permute init demo && cd demo && permute build && permute run
init_build_run=OK
```

The `init` → `build` → `run` path is the real acceptance test: `build` resolves
`abi/permute_iface.h`, `guest/permute_guest.ld` and
`build/libpermute_guest.a` under the installed `PERMUTE_HOME`, and `run` starts
the installed `permute-run` and `permute-worker`.

---

## 3. Uninstall evidence

```sh
$ ./install.sh --uninstall --prefix /tmp/opencode/p401/prefix
install.sh: removed /tmp/opencode/p401/prefix (zero residue under /tmp/opencode/p401/prefix)
residue_files=0
prefix_exists=no
```

Uninstall reads the manifest, deletes only listed paths, removes `.pyc` files,
prunes every empty directory depth-first, and removes the prefix itself only if
it is empty. It never removes a non-empty `bin/` or a shared `/usr/local`.
A second `make uninstall` reports "nothing installed" and exits 0.

Bytecode is not a residue source: the installed launcher sets
`PYTHONDONTWRITEBYTECODE=1`, which the install → run → uninstall test confirms.
The uninstall path still deletes pre-existing `__pycache__` trees for
installations made by older launchers.

---

## 4. Version evidence

`--version` now prints all three required values:

```
permute 0.1.0 (git 32d9151, kernel model 1)
```

- Version comes from `cli/permute/__main__.py`.
- SHA resolution order: `PERMUTE_GIT_SHA` (baked by the installed launcher),
  then `git rev-parse --short HEAD`, then `permute-version.txt` (populated by
  `git archive` for tarballs), then `unknown`.
- Kernel model comes from `codec.MODEL_VERSION`, the same value the trace
  footer carries.

The SHA is resolved by a lazy argparse action, so ordinary commands do not pay
for a `git` subprocess. Measured startup (NFR-12): **84 ms** for `--version`
(limit 200 ms) and **193 ms** for `permute run --seeds 1` (limit 2 s), both
recorded by `make gate2`.

---

## 5. Dist evidence

`make dist` refuses a dirty tree, then runs:

```
git archive --format=tar.gz --prefix=permute-0.1.0/ -o dist/permute-0.1.0.tar.gz HEAD
sha256sum -> dist/SHA256SUMS
```

Verified from the packaged commit:

```
dist: dist/permute-0.1.0.tar.gz (about 1.1 MB, commit <sha>)
dist/SHA256SUMS: OK          # sha256sum -c
tar extract: no .git directory
permute-version.txt: substituted short SHA
tools/permute --version: permute 0.1.0 (git <sha>, kernel model 1)
make build: 100/100 targets
install.sh: installed, init/build/run passed, uninstall left no residue
```

`git archive` substitutes `$Format:%h$` in `permute-version.txt` (via
`.gitattributes` `export-subst`), and `install.sh` falls back to that file when
`.git` is absent. The hash itself is not copied into this report: it is a hash
of the commit that would contain the report, so any committed value would be
stale by construction. `dist/SHA256SUMS` is generated per run and gitignored.

---

## 6. Reproduce

```sh
make
make install PREFIX=/tmp/opencode/prefix
/tmp/opencode/prefix/bin/permute --version
make uninstall PREFIX=/tmp/opencode/prefix
make dist
tar -xzf dist/permute-0.1.0.tar.gz -C /tmp/opencode/tarball
cd /tmp/opencode/tarball/permute-0.1.0 && make build
./install.sh --prefix /tmp/opencode/tarball/prefix
/tmp/opencode/tarball/prefix/bin/permute --version
./install.sh --uninstall --prefix /tmp/opencode/tarball/prefix
```

`make dist` is the only target that requires a clean working tree. `make
install` deliberately does not depend on `build`, so `sudo make install` never
builds as root; it fails with `missing build/… (run make first)` instead.
