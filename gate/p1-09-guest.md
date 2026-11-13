<!-- SPDX-License-Identifier: Apache-2.0 -->

# P1-09 report — guest packaging

**Date:** 2026-11-14
**Scope:** per-process shared objects, `permute_main`, load/start split for `EV_SPAWN` ordering, `dlopen` flags, exported-symbol control, link-order proof.
**Result:** **19 checks, 0 failures**; `make test` green across all suites and both Python suites.

---

## 1. What was built

| Path | Purpose |
|---|---|
| `guest/dlmain.h/.c` | Load, start, unload a guest instance |
| `guest/permute_guest.ld` | Version script: `permute_main` plus the interposed ABI, everything else hidden |
| `tools/make_guest.py` | Parse the interface definition, check the script, compile `proc_<name>__<n>.so`, verify `nm` |
| `tests/fixtures/guest/counter.c` | One program compiled twice; per-instance static and a constructor |
| `tests/unit/test_guest.c` | Isolation, constructor order, ENOSYS probe, missing object |

---

## 2. Results

```
test_guest: 19 checks, 0 failures
all other suites unchanged: 326 + 120149 + 21351 + 277 + 232 + 191 + 20 + 547 + 29
```

| Behaviour | Result |
|---|---|
| Two `.so` files from the same source | distinct handles, distinct `.data` |
| `set 42` in instance 0, then `get` in instance 1 | 42 and **0** |
| Instance 1 sets 7 | instance 0 still 42 |
| Constructors | log reads `0\n1\n` — load order |
| `MsgSend` with no kernel installed | `-1`, `errno == ENOSYS` |
| Missing object | `-1` with a populated `dlopen` error |
| `nm -D --defined-only` | `permute_main`, `ChannelCreate`, `ConnectAttach`, `MsgSend`, `MsgReceive`, `MsgReply` — all `T`, no internals |
| Version script vs `abi/permute_iface.def` | checked on every guest build; drift fails the build |

---

## 3. `dlmopen` was considered and rejected

The task brief mentioned `LM_ID_NEWLM`/`dlmopen` namespace isolation. The requirements reject it explicitly, and ADR-005 records why:

> glibc's per-namespace libc copy is fragile, the namespace count is capped, and a bug there presents as simulator nondeterminism.

So the model is one object per process instance, opened with `RTLD_NOW | RTLD_LOCAL`. That gives the property the requirement actually asks for — genuinely separate `.data`/`.bss`, and correct per-thread TLS for free — without a second libc. The test proves the separation directly: the same source, two objects, and a static written in one is invisible in the other.

If this needs to change, it is ADR-005 that has to change first, not the loader.

---

## 4. The exported surface is generated, not maintained

`tools/make_guest.py` reads `abi/permute_iface.def`, renders the version script, refuses to build if the checked-in script has drifted, and then runs `nm -D --defined-only` on the result. Two failure modes are closed:

- A new interposed symbol added to the definition file but not exported would make the guest call libc's version silently. The drift check catches it at build time.
- An internal helper (for example `permute_ops`) leaking into the dynamic table would let one instance see another's implementation details. The `local: *` clause plus the `nm` check prevents it.

The `nm` output is the proof that `MsgSend` is **defined** in the guest object (`T`), not imported (`U`). That is the link-order requirement stated as an observation.

---

## 5. Load and start are separate

`permute_guest_load` runs the constructors; `permute_guest_start` calls `permute_main`. The split exists so the executor can place each half at the right point in `EV_SPAWN`: constructors in spawn order before the event, `permute_main` when the event is dispatched.

The event loop does not exist yet, so the test proves the property the event will rely on — constructor order equals load order. Recorded as `LIMITS.md` divergence #18 rather than pretending the event exists.

Per-thread TLS across instances is not yet tested (divergence #19): the mechanism is per-object by construction, but the fixture is single-threaded. That test arrives with the executor and the pthread shim.

---

## 6. Next

**P1-10 — deterministic allocator.** A 2 GiB reserved arena with lazy commit, size-class free lists sorted by offset, the full `malloc` family, address-range dispatch for libc pointers, and an RSS ceiling. This is the last piece of the guest-side memory model before the executor can run real service code.
