<!-- SPDX-License-Identifier: Apache-2.0 -->

# P1-08 report — replay

**Date:** 2026-11-14
**Scope:** seed determinism, artifact verification, manifest refusal with exit 4, `--force`, ordering checks, corrupted and partial trace rejection.
**Result:** **29 replay checks plus the 547 codec checks, 0 failures**; `make test` green across all C suites and both Python suites.

---

## 1. What was built

| Path | Purpose |
|---|---|
| `cli/permute/run.py` | `replay seed` and `replay artifact`, exit-code contract, manifest and ordering checks |
| `tools/replay_fixture.c` | Deterministic seeded run through the real kernel, writing a trace |
| `kernel/trace.c` | Footer reserved word now records the kernel model version |
| `cli/permute/trace/codec.py` | Exposes that version |
| `tests/python/test_replay.py` | 29 checks covering both modes and every exit path |

---

## 2. Results

```
test_replay: 29 checks, 0 failures
test_codec: 547 checks, 0 failures
all C suites unchanged: 326 + 120149 + 21351 + 277 + 232 + 191 + 20, 0 failures
```

| Behaviour | Result |
|---|---|
| Seed 42, two separate process launches | byte-identical trace, same digest `5150741151ed0d93`, 716 records |
| Seed 43 | different digest `af3bc48eef1aaf63` |
| Fixture digest vs the trace's own `run_digest` | equal; a fixture that lies about its digest fails the run |
| Artifact with matching manifest | exit 0 |
| Wrong build id / config hash / kernel model | exit 4, refusal names the field |
| Same mismatch with `--force` | exit 0, and the force is recorded in the report |
| Wrong expected digest, even with `--force` | exit 11 |
| Byte flip in a chunk | exit 2, "artifact unusable" |
| Truncated file | exit 2, "incomplete: a partial trace cannot prove a run" |
| Seq regression / time going backwards / missing schema field | caught by `verify_ordering` |
| Artifact carries no wall clock | `created_unix_ns == 0` asserted |

---

## 3. Manifest authority

The comparison is against **caller-supplied** values (`--build-id`,
`--config-hash`, `--kernel`, `--digest`), never against what the file claims
about itself. A trace that names the wrong build is data to check, not an
authority to trust. This is why `--force` can accept a manifest mismatch but
cannot accept a digest mismatch: the manifest is a claim, the digest is the
recomputation.

The kernel model version had no home in the format. It now occupies the
footer's reserved word (IMP-27). Without it, a kernel change would surface as
exit 11 ("digest differs") instead of exit 4 ("kernel mismatch"), and FR-6.6's
exit-code contract would be unimplementable.

---

## 4. What "from seed" means today

There is no run loop yet, so `tools/replay_fixture` provides the mechanism
rather than the product: it seeds the decision stream, creates a server and
two clients, and runs 200 iterations of send→receive→reply plus seeded pulses,
emitting a record for each step. Every varying choice — which client, payload
length, payload bytes, clock advance, pulse code, pulse value — comes from the
decision stream or the virtual clock. It writes `created_unix_ns = 0` and no
host data.

Two runs of seed 42 are **byte-identical**, which is a stronger result than
digest equality. That is the property replay depends on.

This is IMP-29, explicitly a stand-in until P1-12 supplies `permute-run` and
the scenario loader.

---

## 5. Staged, and stated as staged

Exit 10 ("violations differ") and exit 1 ("violation found") require the oracle
engine. Replay today proves integrity, manifest, ordering, and digest; its
output says the violation comparison is not implemented rather than implying it
passed. Recorded as `LIMITS.md` divergence #17.

A partial trace is readable for diagnosis but refused for replay. A truncated
log cannot prove what a run did, and silently accepting it would make "replay
verified" mean less than it says.

---

## 6. Next

**P1-09 — guest packaging.** Per-process shared objects, `permute_main`, spawn
ordering at `EV_SPAWN`, and the link-order test. That begins the move from
kernel-internal tests to running code shaped like a real QNX service, and it is
the prerequisite for the executor.
