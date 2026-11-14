<!-- SPDX-License-Identifier: Apache-2.0 -->

# ABI

**Owner:** [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14)
**Status:** current for v0.1. Derived from `abi/permute_iface.def`, `abi/permute_abi.h` and `guest/permute_guest.ld`.
**Companion documents:** [`ARCHITECTURE.md`](ARCHITECTURE.md) §2 (link model), [`KERNEL-CONTRACT.md`](KERNEL-CONTRACT.md) (what the shim calls), [`LIMITS.md`](LIMITS.md) §2 (what is not interposed).

This is the guest-side ABI: what a subject-under-test object may call, how it is
linked, and what the simulator guarantees in return. It is **not** a kernel ABI
and not binary compatibility with QNX. A guest is recompiled against these
declarations; an existing QNX binary cannot be loaded.

---

## 1. Three contracts

| Contract | Mechanism | Enforced by |
|---|---|---|
| Source compatibility | clean-room declarations in `abi/permute_qnx.h` and `abi/permute_abi.h`; the same source compiles for QNX | dual-target conformance sources |
| Link-time shadowing | `libpermute_guest.a` linked ahead of libc; strong symbols; version script | `make symcheck`, `nm` audit |
| Runtime dispatch | a read-only hypercall page publishes the kernel operation table | `permute_hypercall_bind`, magic + version check |

The shim never implements semantics. Every function forwards to the operation
table; a missing entry is a hard `ENOSYS`, never a silent approximation.

---

## 2. The interposed surface

`abi/permute_iface.def` is the single source of truth. It currently contains
**33 symbols in six groups**. Groups are atomic: a group is shadowed entirely
or not at all, because mixing one half of a primitive with libc's other half
corrupts state.

### 2.1 IPC — 13 symbols

| Symbol | Prototype |
|---|---|
| `ChannelCreate` | `int ChannelCreate(unsigned flags)` |
| `ChannelDestroy` | `int ChannelDestroy(int chid)` |
| `ConnectAttach` | `int ConnectAttach(uint32_t reserved, pid_t pid, int chid, unsigned index, int flags)` |
| `ConnectDetach` | `int ConnectDetach(int coid)` |
| `MsgSend` | `long MsgSend(int coid, const void *smsg, size_t sbytes, void *rmsg, size_t rbytes)` |
| `MsgReceive` | `rcvid_t MsgReceive(int chid, void *msg, size_t bytes, struct _msg_info *info)` |
| `MsgReply` | `int MsgReply(rcvid_t rcvid, long status, const void *msg, size_t bytes)` |
| `MsgError` | `int MsgError(rcvid_t rcvid, int error)` |
| `MsgSendPulse` | `int MsgSendPulse(int coid, int priority, int code, long value)` |
| `MsgReceivePulse` | `int MsgReceivePulse(int chid, void *pulse, size_t bytes, struct _msg_info *info)` |
| `MsgRegisterEvent` | `int MsgRegisterEvent(struct sigevent *event, int coid)` |
| `MsgDeliverEvent` | `int MsgDeliverEvent(rcvid_t rcvid, const struct sigevent *event)` |
| `MsgVerifyEvent` | `int MsgVerifyEvent(rcvid_t rcvid, const struct sigevent *event)` |

`MsgSendv`, `MsgReceivev`, `MsgReplyv` and `MsgInfo` are **not** in the surface
today. `LIMITS.md` §3 records the gap; the ABI does not pretend otherwise.

### 2.2 Identity — 2 symbols

| Symbol | Prototype | Value |
|---|---|---|
| `getpid` | `pid_t getpid(void)` | deterministic ordinal, never the host pid |
| `gettid` | `pid_t gettid(void)` | deterministic ordinal |

### 2.3 Threads — 6 symbols

| Symbol | Prototype |
|---|---|
| `pthread_self` | `pthread_t pthread_self(void)` |
| `pthread_equal` | `int pthread_equal(pthread_t a, pthread_t b)` |
| `pthread_create` | `int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)` |
| `pthread_join` | `int pthread_join(pthread_t thread, void **retval)` |
| `pthread_exit` | `void pthread_exit(void *retval)` |
| `pthread_cancel` | `int pthread_cancel(pthread_t thread)` |

Thread tokens are 64-bit ordinals, never addresses. Mutex, condition variable,
rwlock, semaphore, once and barrier functions are **not** interposed; they
resolve to libc, which is why a scenario that depends on them is compiled and
skipped rather than run (LIMITS #37).

### 2.4 Time — 5 symbols

| Symbol | Prototype |
|---|---|
| `clock_gettime` | `int clock_gettime(clockid_t clock_id, struct timespec *ts)` |
| `time` | `time_t time(time_t *out)` |
| `gettimeofday` | `int gettimeofday(struct timeval *tv, void *tz)` |
| `nanosleep` | `int nanosleep(const struct timespec *req, struct timespec *rem)` |
| `TimerTimeout` | `int TimerTimeout(clockid_t id, uint32_t flags, const struct sigevent *event, const uint64_t *ntime, uint64_t *otime)` |

The clock is virtual. `clock_gettime` on a real clock id would break
determinism, so the shim routes every call to the kernel's virtual clock. The
determinism gate (`make symcheck`) fails if production code calls a real clock.

### 2.5 Entropy — 4 symbols

| Symbol | Prototype |
|---|---|
| `rand` | `int rand(void)` |
| `srand` | `void srand(unsigned seed)` |
| `random` | `long random(void)` |
| `srandom` | `void srandom(unsigned seed)` |

Entropy is a per-process stream derived from the run seed; libc's generator is
never used. Outside a run the stream is seeded with zero, which is
deterministic but meaningless (LIMITS #25).

### 2.6 Scheduling — 3 symbols

| Symbol | Prototype |
|---|---|
| `SchedSet` | `int SchedSet(pid_t pid, int tid, int policy, const struct sched_param *param)` |
| `SchedGet` | `int SchedGet(pid_t pid, int tid, struct sched_param *param)` |
| `SchedYield` | `int SchedYield(void)` |

`SchedGet` maps the effective priority into `sched_priority`, because the host
struct has no `sched_curpriority` field (IMP-52).

---

## 3. Types and layouts

| Type | Definition | Why |
|---|---|---|
| `rcvid_t` | `int32_t` | receive ids are 32-bit in the target interface |
| `permute_handle_t` | `uint64_t` | slot plus generation; a stale handle is rejected, not reinterpreted |
| thread token | `uint64_t` | ordinal, never a pointer |
| `struct _msg_info` | public layout, asserted in tests | field order and widths are from public documentation |
| `struct sigevent` | 32 bytes on LP64, `_Static_assert` | QNX layout, not glibc's |
| `struct _pulse` | 24 bytes on LP64, `_Static_assert` | 64-byte fixed message body includes code and value |

The clean-room rule is strict: no vendor header is included or copied. Values
in `abi/permute_qnx.h` come from public interface documentation. A signature
test compiled against real headers is the mitigation when an SDP is present
(ADR-011).

---

## 4. The hypercall page

```c
typedef struct permute_hypercall_page {
    uint32_t magic;            /* 0x504D5448, "PMTH" */
    uint16_t version;          /* 1 */
    uint16_t flags;
    uint32_t page_size;
    uint32_t reserved0;
    const permute_kernel_ops_t *ops;
    uint64_t reserved[3];
} permute_hypercall_page_t;
```

- 1 MiB is reserved at a candidate base; one page is used. The candidate list
  starts at 256 GiB so the same object works in a 39-bit user address space.
- Host: `permute_hypercall_init()` maps the page and returns its base.
- Guest: `permute_hypercall_bind(base)` validates magic and version, then sets
  the module's view. Every guest module must bind; a module that does not sees
  a null page and fails every call with `ENOSYS`.
- `permute_ops()` reads the page, never a mutable global, so a stray assignment
  cannot redirect the call path.

The operation table itself is specified in
[`KERNEL-CONTRACT.md`](KERNEL-CONTRACT.md).

---

## 5. Deterministic allocator

The guest archive additionally defines `malloc`, `free`, `calloc`, `realloc`,
`posix_memalign` and `strdup` (glibc only). These are not QNX API and are not
part of the 33-symbol audit; they are the allocator contract.

- A 2 GiB region is reserved at a fixed base and committed in 2 MiB windows.
- Blocks come from a bump pointer and per-size-class free lists ordered by
  offset, so the address sequence is a function of allocation history.
- Requests above 2 MiB return `ENOMEM` rather than falling back to libc
  (LIMITS #20).
- Pointers outside the arena belong to libc; `free` and `realloc` forward them
  rather than corrupt them.
- On musl the interposition is disabled with
  `-DPERMUTE_NO_INTERPOSED_ALLOC=ON`; the allocator core is still tested
  through a prefixed object (LIMITS #35).

---

## 6. Link line

A guest object is built with:

```sh
cc -std=c11 -O2 -fPIC -shared -D_GNU_SOURCE \
   -I <repo> -DGUEST_INSTANCE=<n> \
   -Wl,--version-script=guest/permute_guest.ld \
   -Wl,-Bsymbolic-functions \
   -Wl,-u,permute_hypercall_bind \
   -Wl,--no-undefined \
   -o proc_<name>__<n>.so <source> -lpermute_guest
```

| Flag | Reason |
|---|---|
| `-Wl,--version-script` | exports `permute_main` and the interposed names, hides everything else |
| `-Wl,-Bsymbolic-functions` | binds libc's internal calls to libc; without it libc would preempt the shims and call the host |
| `-Wl,-u,permute_hypercall_bind` | retains the bind symbol so the loader cannot discard it |
| `-Wl,--no-undefined` | a missing symbol is a link error, not a runtime surprise |

One object is built per process instance. `dlopen(RTLD_NOW | RTLD_LOCAL)`
gives each instance its own `.data`, `.bss` and TLS, and keeps its symbols out
of the global namespace. `RTLD_GLOBAL` would let one instance's shim resolve
another's, destroying process isolation (ADR-005).

The entry point is `int permute_main(int argc, char **argv)`. On a real target
the same source is wrapped by a target `main`; on the simulator the loader
calls `permute_main`.

---

## 7. Audit and change procedure

```
make symcheck        # every symbol in the def is defined by libpermute_guest.a
python3 tools/make_guest.py --check-linker-script   # the version script matches the def
```

To add a symbol:

1. Add one `PERMUTE_SYMBOL(...)` line to `abi/permute_iface.def`.
2. Implement the shim next to its group (`abi/shim_*.c`), forwarding only.
3. Add the kernel entry in `permute_kernel_ops_t` and the bridge.
4. Add a unit test that proves the forwarding and the error path.
5. Run `make symcheck` and `--check-linker-script`; both must pass.

Removing a symbol is a group decision: a partially shadowed group is rejected
by the audit (FR-2.4).

---

## 8. Versioning

The hypercall page carries `version` and the trace header carries
`PERMUTE_KERNEL_MODEL_VERSION`. v0.1 makes no ABI stability promise: a guest
object is rebuilt for the release that runs it. A future version that changes
either number must reject a mismatched guest at bind time rather than run it.

---

## 9. What is deliberately outside this ABI

- **Mutexes, condition variables, rwlocks, semaphores, barriers** — libc
  resolves them; the executor models threads, not every synchronisation
  primitive. Recorded in `LIMITS.md` §3.
- **Files, `stdio`, pathnames** — no `open`/`read`/`write`/`printf`
  interposition. A guest that touches the host filesystem is outside the claim.
- **Signals as asynchronous interrupts** — `SIGEV_SIGNAL*` is rejected rather
  than approximated.
- **Dispatch and resource managers** — dropped after the P2-11 kill criterion;
  no `dispatch_*`, `resmgr_attach`, `iofunc_*` or `name_attach` (DECISIONS
  §4.1).
- **Vectors and `_msg_info` convenience wrappers** — `MsgSendv`, `MsgReplyv`,
  `MsgInfo` are not in the surface.

A guest that calls one of these gets libc behaviour. That is a fidelity gap,
not a supported path, and `LIMITS.md` lists it rather than hiding it.
