<!-- SPDX-License-Identifier: Apache-2.0 -->

# P0-05 report — hypercall page

**Date:** 2026-11-14
**Commit:** `b129be4` + this work
**Scope:** publish the kernel operation table at a probed fixed virtual address; remove the mutable global from the call path.
**Result:** **three modes pass, 100/100 runs stable, 3.1–3.6 ns per shim call.**

---

## 1. What was built

| Path | Purpose |
|---|---|
| `guest/hypercall.h/.c` | Candidate list, page layout, `init`, `bind`, `ops`, `ops_install` |
| `abi/shim_ipc.c` | Every shim now reads the active table via `permute_ops()`; no global to assign into |
| `abi/permute_abi.h` | Removed the public mutable pointer; the page is the only publication mechanism |
| `tests/spike_hypercall.c` | Three modes plus a cost benchmark |
| `tools/hypercall_check.sh` | Stability across 100 processes, probe behaviour, fallback |

---

## 2. Results

```
MODE=default        HYPERCALL=0x0000004000000000   (256 GiB, candidate 0)
MODE=occupy-first   HYPERCALL=0x0000010000000000   (1 TiB, candidate 1)
MODE=fallback       HYPERCALL=fallback
```

| Check | Result |
|---|---|
| Page claimed at the first candidate | pass |
| Page magic and version valid at that address | pass |
| Shim reads the table through the page | pass |
| Clearing the table yields `-1/ENOSYS` | pass |
| Reinstalling restores the call path | pass |
| Occupied candidate is skipped | pass (candidate 0 → candidate 1) |
| Fallback works with fixed mappings disabled | pass |
| Address stable across 100 separate processes | **100/100** |
| Dispatch cost | **3.07 / 3.14 / 3.63 ns per call** (10⁷ iterations each) |

`make hypercall` reproduces all of it.

---

## 3. Why the probe is tested, not just implemented

A hardcoded address passes every "the page is at the expected address" test. The only way to know the probe works is to make the first candidate unavailable and observe the move:

```
$ ./build/spike_hypercall --occupy-first
MODE=occupy-first
HYPERCALL=0x0000010000000000
PASS  probe skipped the occupied candidate
```

The same reasoning applies to the stability check: it compares **separate processes**, not two calls in one process, because a per-process constant would satisfy the weaker version.

---

## 4. The design decision that mattered

The obvious implementation — replace a global pointer with a page pointer — breaks at P1-09.

Each simulated process is its own shared object with its own statics. The host initialises the page once; every guest module still has a null local pointer unless something tells it where the page is. That produces a uniform `ENOSYS` from every call, which looks like a missing kernel rather than a binding bug.

So the API has two phases:

- `permute_hypercall_init()` — host, once. Maps, stamps the magic, binds locally.
- `permute_hypercall_bind(base)` — guest module, once per module. Validates the magic and adopts the page.

The magic check is what makes a wrong address a loud failure instead of undefined behaviour. `bind()` is used today by `init()`; at P1-09 the host calls it after each `dlopen`.

Fallback remains available (`PERMUTE_NO_HYPERCALL=1`) for environments that deny fixed mappings, and reports itself as `base == 0` so nothing downstream has to guess which mode is active.

---

## 5. Cost

The page adds one load and one null test per shim call. Measured over 10⁷ calls:

```
NS_PER_CALL=3.07
NS_PER_CALL=3.14
NS_PER_CALL=3.63
```

That is the dispatch overhead only — the kernel work (arena copy, state transition) is still to come. The budget is 60–150 ns for the whole non-blocking path, so dispatch is ~5% of it at worst.

A syscall in this path would cost ≥ 100 ns, so the measurement also rules out an accidental syscall better than a trace would: it is three orders of magnitude away from the number.

---

## 6. What is not covered yet

| Gap | Consequence | Task |
|---|---|---|
| Per-module binding not exercised with real `.so` files | The seam exists and is tested in-process; the multi-.so case is untested | P1-09 |
| Chosen base not written to a run manifest | Reproducibility metadata incomplete | P1-13 |
| Address recorded but not checked against a hostile mapping | `MAP_FIXED_NOREPLACE` prevents clobbering; a pre-existing occupant just moves us to the next candidate | — |
| No guard page between page and rest of the reservation | A wild write past the page would silently corrupt reserved memory | P1-02, when kernel arenas land |

---

## 7. Next

**P0-04 completion** (extend the surface only as tests appear), then **P1-01/P1-02** — the kernel object model and the IPC core. The two blocking external dependencies remain the SDP (P0-07, P0-08) and the staffing decision (OD-02); everything on the host side is now unblocked through the first vertical slice.
