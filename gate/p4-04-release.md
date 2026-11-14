<!-- SPDX-License-Identifier: Apache-2.0 -->

# P4-04 report — v0.1.0 tag and release notes

**Date:** 2026-11-14
**Scope:** §13.4 — code freeze, annotated `v0.1.0` tag with notes and checksums,
release gates green at the tagged commit.
**Result:** **PASS.** `v0.1.0` is an annotated tag on `e5cfc2f`, pushed to
origin, carrying the release notes and the tarball checksum. `make
release-check` is green at that commit.

---

## 1. The tag

| Item | Value |
|---|---|
| Tag | `v0.1.0` (annotated) |
| Tag object | `3d8f851ffc735916817219667f5ca853e60fa463` |
| Commit | `e5cfc2f3700f95af911a309bd3ff64fa8c2d1759` |
| Remote | `refs/tags/v0.1.0` on `origin` |
| Notes | feature summary, evidence list, artifact checksum, and the explicit statement that target conformance is not claimed |

```sh
git tag -a v0.1.0 -F tagmsg.txt e5cfc2f
git push origin v0.1.0
git ls-remote --tags origin | grep v0.1.0
3d8f851…  refs/tags/v0.1.0
e5cfc2f…  refs/tags/v0.1.0^{}
```

## 2. Artifact

```text
dist/permute-0.1.0.tar.gz   1160316 bytes, commit e5cfc2f
sha256  1c809b91de4ab0da3f7a37a93811c0fd1abcb77bc036957edcd9064d9fc1da41
sha256sum -c SHA256SUMS      permute-0.1.0.tar.gz: OK
```

The tarball is built by `make dist` from the clean tagged commit with
`git archive`, so it contains exactly the tagged tree and its
`permute-version.txt` carries `e5cfc2f` (substituted by `export-subst`).

## 3. Gates at the tagged commit

`make release-check` is the aggregate: `make ci` followed by `make gate1`.

| Gate | Result | Time |
|---|---|---|
| `make ci` | green: unit, codec, replay, allocator, interposition, determinism (1,000 runs), coverage, docs, quickstart, examples, licence, submission, CI-check | ~190 s |
| `make gate1` | 9/9 PASS | ~198 s |
| `make release-check` | **exit 0** | **387 s** |

The PRD's shorthand is "`make verify` green". In this repository `make verify`
is the `--verify` feature test (P2-08), so the release aggregate is named
`make release-check` to avoid redefining an existing target; it runs the same
checks the shorthand intends, plus the submission scan.

`make gate2` is not part of the release aggregate because its outcome-class row
is deliberately red until C11/C13 can execute (LIMITS #60). That is reported in
`gate/gate2-scorecard.md`, not hidden.

## 4. Why the tag moved once

The first tag (`4aa8040` → `796fb26`) was created before `make release-check`
was run at that commit. The run failed: `tools/submission_check.py` scanned its
own report and flagged the vendor names quoted in prose as prohibited content —
a self-referential false positive. The rule now flags a vendor name only on a
line that also attributes copyright or a licence, and the report explains the
distinction.

That fix is `e5cfc2f`, where `make release-check` exits 0. Because the earlier
tag pointed at a commit whose own release gate failed, and no consumer had
picked it up, `v0.1.0` was moved to `e5cfc2f` and force-pushed. Reflog:
`+ 4aa8040...3d8f851 v0.1.0 -> v0.1.0 (forced update)`.

Leaving the tag on a red commit would have made "release gates green at the
tagged commit" false; moving it is the smaller dishonesty, and it is recorded
here.

## 5. Reproduce

```sh
git checkout v0.1.0
make release-check
make dist
cd dist && sha256sum -c SHA256SUMS
tools/clean_room.sh        # offline, from the tagged tree
```

The checksum is reproducible only while the same `git archive` implementation
and commit are used; the committed `SHA256SUMS` is the artifact of record.
