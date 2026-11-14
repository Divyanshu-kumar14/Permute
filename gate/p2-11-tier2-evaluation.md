<!-- SPDX-License-Identifier: Apache-2.0 -->

# P2-11 report — Tier 2 kill criterion

**Date:** 2026-11-14
**Scope:** can the canonical QNX "Bones of a Resource Manager" skeleton run
unmodified under the simulator?
**Result:** **no.** The probe does not compile, and the runtime has none of the
required surface. **FR-7.2/7.3 are dropped**, FR-7.1 goes with them, FR-7.5 is
recorded as a deviation, and `name_attach` (FR-7.4) is the only retained API.
P3-09 does not fire.

**Timebox:** the evaluation finished in one session, well inside the 8-hour
box. The blocker is structural, so more time would not have changed the answer.

---

## 1. The probe

| File | What it is |
|---|---|
| `scenarios/s05_resmgr_devctl/skeleton.c` | the canonical QNX 8.0 "Bones of a Resource Manager" source, unmodified |
| `scenarios/s05_resmgr_devctl/client.c` | a client that opens `/dev/hello` and issues a `devctl` |
| `scenarios/s05_resmgr_devctl/README.md` | why the files are not named `scenario.c` |

The skeleton uses exactly the APIs the kill criterion names: `dispatch_create`,
`dispatch_context_alloc`, `dispatch_block`, `dispatch_handler`,
`resmgr_attach`, `iofunc_func_init`, `iofunc_attr_init`.

---

## 2. Evidence

Build through the same path every scenario uses:

```
$ python3 tools/make_guest.py --source scenarios/s05_resmgr_devctl/skeleton.c \
      --name s05_resmgr --instance 0 --out build/tier2 --build-dir build
scenarios/s05_resmgr_devctl/skeleton.c:10:10: fatal error:
    sys/iofunc.h: No such file or directory
compilation terminated.
```

The client fails at the same include, before `devctl`.

```
$ grep -E "open|devctl|name_|dispatch|resmgr|iofunc" abi/permute_iface.def
(no matches)

$ nm build/libpermute_core.a | grep -E " T (dispatch_|resmgr_|iofunc_|name_)"
(no matches)

$ find abi -name dispatch.h -o -name iofunc.h -o -name name.h
(no matches)
```

The interposition surface is 34 symbols: IPC, time, identity, scheduling,
pthread. There is no `open`, `devctl`, `name_attach`, `dispatch_*`,
`resmgr_attach` or `iofunc_*`. The simulator is a single guest process, so a
pathname namespace has no process boundary to cross and no `open` shim to
resolve it.

---

## 3. Why this is a drop, not a small adapter

Even with hand-written headers, the skeleton needs:

| Missing layer | Consequence |
|---|---|
| pathname registry and longest-prefix matching | `resmgr_attach("/dev/hello")` has nowhere to register |
| `open`/`devctl` interposition | a client cannot reach the server |
| dispatch loop over one or more channels | `dispatch_block` has no kernel call to wait on |
| `_IO_CONNECT`/`_IO_DEVCTL` parsing | `dispatch_handler` has no message contract |
| `iofunc_*` defaults and OCB lifetime | the skeleton's tables are unpopulated |

That is the Tier 2 implementation task (P3-09), not a conformance adapter. The
kill criterion exists to avoid spending that cost without evidence; the
evidence says the current scope cannot carry it.

---

## 4. Decision

| Requirement | Outcome |
|---|---|
| FR-7.1 `dispatch_*` | dropped — no retained consumer without `resmgr_attach` |
| FR-7.2 pathname namespace | **dropped** |
| FR-7.3 `iofunc_*` defaults | **dropped** |
| FR-7.4 `name_attach` / `name_open` | retained as the only resource-manager-adjacent API; **not implemented** |
| FR-7.5 `dispatch_handler` non-preemption | not implemented; recorded deviation (D-15, §18.2 X10) |
| P3-09 | does not fire; its 8 hours stay in the buffer |

Recorded in [`docs/DECISIONS.md`](../docs/DECISIONS.md) §4.1 and
[`docs/LIMITS.md`](../docs/LIMITS.md) #81–#83. `name_attach` remains a
possible later addition with a sorted process-global name table; it does not
need the namespace.

---

## 5. Verification

```
make ci      green
make gate1   9/9 PASS, exit 0
```

The probe files are not named `scenario.c`, so `make scenarios` does not try to
build them. Adding them changed no target and no scorecard row.
