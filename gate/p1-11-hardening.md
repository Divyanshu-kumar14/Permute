<!-- SPDX-License-Identifier: Apache-2.0 -->

# P1-11 report — determinism hardening

**Date:** 2026-11-14
**Scope:** banned-container and real-time gate, identity/time/entropy shims, 10⁵ cross-language vectors, decision locality, vendor-duplicate detection.
**Result:** **30 shim checks, 86 vector checks, 9 gate checks, 100,000 C vectors, 100,000 Python vectors, vendor-duplicate gate — 0 failures**; `make test` green.

---

## 1. What was built

| Path | Purpose |
|---|---|
| `tools/symcheck.py` | Symbol audit plus the determinism gate (replaces the shell script) |
| `abi/shim_proc.c` | `getpid`, `gettid` ordinals |
| `abi/shim_pthread.c` | `pthread_self` token, `pthread_equal` |
| `abi/shim_time.c` | `clock_gettime`, `time`, `gettimeofday`, `nanosleep` |
| `abi/shim_rand.c` | `rand`/`srand`/`random`/`srandom` from a per-process origin |
| `tests/golden/rng_vectors.tsv` | 100,000 committed vectors |
| `tools/gen_rng_vectors.py` | Regenerates the vector file |
| `tests/fixtures/vendor_dup/` | Vendor archive defining `MsgSend`, and a link probe |
| `tools/vendor_dup_check.sh` | Asserts the link fails and names both objects |

---

## 2. Results

```
test_shim:       30 checks, 0 failures
test_rng_golden: 86 checks, 0 failures (100,000 vectors)
test_determinism_gate: 9 checks, 0 failures
test_rng:         6 checks, 0 failures (100,000 vectors)
symcheck: PASSED (17 symbols defined)
determinism gate: PASSED
vendor-dup: PASSED (linker named both objects)
```

| Behaviour | Result |
|---|---|
| `getpid`/`gettid` | return the kernel's ordinals (4242 / 77 in the fixture) |
| `pthread_self` | returns the kernel's 64-bit token, not an address |
| `clock_gettime` | returns the virtual time supplied by the kernel |
| `time`, `gettimeofday` | derived from the same virtual clock |
| No kernel table | every shim returns `-1`/`ENOSYS`, except `pthread_self` which returns 0 |
| `srand(7); rand(); srand(7); rand()` | equal |
| `srand(8)` | different stream |
| `rand` / `random` | share one stream |
| 100,000 vectors, C implementation | all match the committed file |
| 100,000 vectors, Python implementation | all match the same file |
| Forcing index 17 | 18..N unchanged; timer domain unaffected by 1,000 scheduling draws |
| Planted `clock_gettime` / `rand` / `hcreate` / `getpid` | gate fails, naming file, line, and category |
| Same calls inside a comment | gate passes |
| Vendor archive defining `MsgSend` | link fails, diagnostic names `shim_ipc.c.o` and `vendor.c.o` |

---

## 3. The bug this task existed to catch

The packaging test hung. `nanosleep` was passed an absolute timestamp taken from the real clock, so it slept for 54 years.

The cause was not the shim. `nm -D` showed `nanosleep` **defined** in the guest object, but `LD_DEBUG=bindings` showed the call resolving to **libc**:

```
binding file ./build/proc_counter__0.so to /usr/lib/libc.so.6: normal symbol `nanosleep'
```

A `dlopen`ed object's references are resolved in the global scope first, and libc is in that scope. The object's own definition loses unless the object is linked with `-Bsymbolic-functions`. Without that flag, every guest would have read the **host clock** while every source-level check passed: the determinism gate scans code, and the code was correct.

Fix: link guest objects with `-Wl,-Bsymbolic-functions` (IMP-35). The probe now also asserts that `clock_gettime`, `time`, `gettimeofday`, and `nanosleep` all fail with no kernel table — if the shim is ever preempted again, the probe returns `100 + n` instead of `-1` and the test fails.

This is the class of defect the project is about: silent, environment-dependent, and invisible until someone looks at the binding.

---

## 4. What the gate enforces, and what it deliberately does not

| Category | Caught | Rationale |
|---|---|---|
| `container` | `hcreate`, `hsearch`, `hdestroy`, `unordered_map`, `std::map`, `uthash` | Iteration order becomes observable |
| `entropy` | `rand`, `srand`, `random`, `srandom`, `getrandom`, `arc4random`, `/dev/urandom`, `RAND_bytes` | Host entropy cannot enter a run |
| `time` | `clock_gettime`, `gettimeofday`, `time`, `nanosleep`, `clock_nanosleep`, `timer_create`, `timer_settime` | Every clock read must be virtual |
| `identity` | `getpid`, `gettid`, `pthread_self` | Host identity leaks host state |

The allowlist is **by path**: `abi/shim_time.c`, `abi/shim_proc.c`, `abi/shim_pthread.c`, `abi/shim_rand.c`. An in-source suppression comment could be added anywhere by anyone; a path allowlist is one line in one file and shows up in review.

Comments are stripped before scanning, preserving line count, so documentation mentioning `clock_gettime` does not trip the gate and reported lines still point at the right source line.

---

## 5. Vectors and locality

100,000 vectors are committed. The C test and the Python test both read **that file**, so a shared bug in either implementation cannot hide behind agreement with the other.

Decision locality is a property of the decision function, not of the stream object: `rnd(seed, domain, index)` takes no mutable state, so substituting the value chosen at index 17 cannot change index 18. The test states that directly, then checks that 1,000 scheduling draws do not shift the timer domain. That is what lets the shrinker force one decision without invalidating the rest of the run.

---

## 6. Vendor duplicates

A vendor archive that defines `MsgSend` must fail the link loudly (FR-2.6). The fixture proves the diagnostic names **both** objects:

```
multiple definition of `MsgSend'; .../libpermute_guest.a(shim_ipc.c.o): first defined here
.../libvendor_dup.a(vendor.c.o): ... first defined here
```

The probe references `vendor_marker` so the vendor object is actually pulled from its archive, and `ChannelCreate` so ours is pulled too. Without that, the linker would never load one of the objects and the duplicate would go unnoticed.

---

## 7. Boundary

| In scope | Out of scope |
|---|---|
| Identity, time, entropy routing | `pthread_create` (needs the executor, #23) |
| Gate over `kernel/`, `abi/`, `guest/` | Signals (FR-4.10, #24) |
| 17 interposed symbols | The remaining 24 in the 41-symbol surface |
| Committed vectors + generator | Fuzzing the generator itself |

`rand` without a kernel table uses seed 0 (#25): it cannot report an error, and a real run always installs the table.

---

## 8. Next

**P1-12 — `permute-run` and `permute-worker`.** Batch claiming, leases, crash isolation, the wall budget, and partial reports. That is the first executable that can run a seed end to end, and it is where the executor finally owns the ready queue.
