<!-- SPDX-License-Identifier: Apache-2.0 -->

# P1-10 report — deterministic allocator

**Date:** 2026-11-14
**Scope:** 2 GiB reserved arena, lazy commit, size-class free lists ordered by offset, the malloc family, address-range dispatch, RSS ceiling.
**Result:** **174 checks + 6 probe checks, 0 failures**; 10⁶ operations deterministic; RSS 14.9 MB against a 70 MB ceiling; `make test` green.

---

## 1. What was built

| Path | Purpose |
|---|---|
| `abi/alloc.h/.c` | Arena, classes, free lists, the malloc family, interposition |
| `tests/unit/test_alloc.c` | Core API, reuse order, determinism, RSS |
| `tools/alloc_probe.c` | Interposed build: the whole process uses the arena |
| `tests/python/test_alloc_probe.py` | Probe determinism and RSS assertion |

---

## 2. Results

```
test_alloc:       174 checks, 0 failures
test_alloc_probe:   6 checks, 0 failures
all other suites unchanged and green
```

| Behaviour | Result |
|---|---|
| 2 GiB reserved at 2 TiB | mapped `PROT_NONE`, not resident |
| 10⁶ mixed alloc/free, same seed twice | identical digest and identical high-water |
| Different seed | different digest |
| Free three blocks out of order, allocate three | returned lowest, then middle, then highest offset |
| `realloc` grow | contents preserved |
| `realloc` shrink within a class | same pointer, contents preserved |
| `posix_memalign(64/4096)` | aligned; bad alignments return `EINVAL` |
| `calloc` | zeroed |
| Double free | error counter increments, no corruption |
| Foreign pointer to core `free` | error counter increments, no corruption |
| Allocation above 2 MiB | `NULL` + `ENOMEM` |
| Same sequence, different history (alloc/free/alloc vs alloc) | same address and same high-water |
| RSS after 10⁶ operations | **14.9 MB** (ceiling 70 MB) |
| libc pointer through interposed `free` | `OWNS_LIBC=0`, process survives |

---

## 3. What makes the addresses deterministic

The arena is reserved once and never moves. Blocks come from a bump pointer, and freed blocks go back to a per-class list kept in ascending offset order, so an allocation always returns the **lowest-addressed free block of its class**. Two runs of the same sequence therefore produce the same offsets whether or not the frees happened between the allocations:

```
history A: malloc(100), free, malloc(100)  → offset X, high-water Y
history B: malloc(100)                      → offset X, high-water Y
```

Both are asserted. That is the property "runs do not differ by allocation history" from FR-4.6, stated as a test rather than a comment.

Absolute addresses are not part of the contract. Invariant 2 forbids pointer values in traces, hashes, and ordering comparisons, so the digest folds **offsets**. If the fixed base is occupied, the allocator maps elsewhere and everything computed from offsets still agrees; the manifest records the base actually used (IMP-32).

---

## 4. Address-range dispatch

A guest legitimately receives libc pointers — `getline`, `realpath`, `dlopen` and friends allocate out of libc's heap. `free` and `realloc` therefore ask whether the pointer is inside the arena and forward to `__libc_free`/`__libc_realloc` when it is not.

The probe proves the dispatch rather than assuming it: it takes a pointer from `__libc_malloc`, asserts the allocator does **not** claim it, and frees it through the interposed `free`. A wrong answer here corrupts both heaps, and the failure would surface far from the cause.

The core API refuses foreign pointers and increments an error counter instead of forwarding, because the core has no business calling libc. Forwarding is the interposer's job.

---

## 5. Boundaries

| In scope | Out of scope |
|---|---|
| Arena, classes, free lists, malloc family, dispatch | `mmap`/`shm_open` interposition (later, FR-4 group `mem`) |
| 16-byte header, 16-byte alignment | `malloc_usable_size`, `mallopt`, `malloc_trim` |
| Requests up to 2 MiB | Larger requests: `ENOMEM` (divergence #20) |
| RSS checked in the normal build | RSS under ASan (divergence #22: shadow memory makes it meaningless) |
| Free lists ordered by offset | O(log n) middle insertion; the list is O(n) there (#21) |

The allocator is not yet linked into `permute_guest` by default. The archive object is only pulled in when a guest references `malloc`, which is exactly the right behaviour — but it means the guest-side interposition is not yet exercised end to end. That lands with the executor, when a guest actually allocates.

---

## 6. Next

**P1-11 — determinism hardening.** The banned-container grep gate, time-routing allowlist, pthread/thread-ordinal shims, 10⁵ cross-language RNG vectors, the decision-locality test, and vendor-duplicate-symbol detection. That is the task that turns the determinism invariants from prose into build failures.
