<!-- SPDX-License-Identifier: Apache-2.0 -->

# QNX evidence

**Owner:** [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14)
**Status:** **build and run verified on QNX 8.0.** The 14 dual-target
scenarios cross-build with `qcc -Vgcc_ntox86_64` and all 14 pass inside a
booted QNX 8.0 x86_64 image. **Oracle traces are still absent:** no
`permute-capture` agent exists, so no `C*.jsonl` comparison against a QNX
reference has been made. Every conformance report therefore still says
`oracle: simulator reference (not a QNX capture)`.

Raw artifacts: [`docs/evidence/qnx/`](evidence/qnx/) — `build.log`,
`serial.log`, `qemu.log`, `manifest.log`.

---

## 1. Required evidence (PRD §17, row 4)

| Field | Required content | State |
|---|---|---|
| SDP version | `QNX Software Development Platform 8.0`, build id | **8.0.0**, baseline `com.qnx.qnx800/8.0.0.00141T202311271501L`, licence `LBTS-26KV-XB3U-TC4D-TJ6M` (non-commercial single-user) |
| Toolchain | `qcc` version from the SDP | **`gcc_ntox86_64` 12.2.0** (`qcc` at `$QNX_HOST/usr/bin/qcc`) |
| `uname -a` | target kernel string from the booted image | **`QNX permute 8.0.0 2026/10/30-09:10:05EDT x86pc x86_64`** |
| Image SHA-256 | hash of the QEMU disk/IFS image used | disk `acadabf24e4fa1a716b7cfe944b20f3f5518cc7844615fb2b15c4373edd2b695`; IFS `0331417ee458fe6ead6e3359edca1015563ee887c3ac739d0a448da428a70de1` |
| QEMU invocation | full `qemu-system-x86_64` command line | [`docs/evidence/qnx/qemu.log`](evidence/qnx/qemu.log) — QEMU 11.1.3, KVM, 4 vCPU, 2 GB |
| Serial log excerpt | boot plus run, timestamps visible | [`docs/evidence/qnx/serial.log`](evidence/qnx/serial.log) — 14 `RESULT ... rc=0` lines between the `PERMUTE CONFORMANCE` markers |
| Commit SHA | the commit the target binaries were built from | `f9ab9ed502aa862181380b7350fe7a8c6dbe9954` plus the scenario fixes in section 3 |
| Oracle traces | `conformance/oracle/qnx/C*.jsonl` + `C*.outcome` + `manifest.toml`, captured on QNX | **absent** — no capture agent; see section 4 |
| `permute-capture` | built with `qcc -Vgcc_ntox86_64` and run inside QNX 8.0 | **not built** — not implemented (PRD ADR-010) |

The build-and-run halves of PRD §17 row 4 are satisfied. The oracle-trace
half is not, and no claim of target fidelity is made on its behalf.

## 2. What the run proves

`tools/qnx_build_scenarios.sh` cross-builds every `conformance/scenarios/c*.c`
against the vendor headers selected by `__QNXNTO__`, and the binaries are baked
into the image's system partition. They execute under `procnto-smp-instr` and
return 0:

```
c01_request_reply            rc=0
c02_priority_order           rc=0
c03_fifo_equal               rc=0
c04_pulse_vs_message         rc=0
c05_pulse_while_processing   rc=0
c06_pulse_only               rc=0
c07_timeout_unblock_esrch    rc=0
c09_reply_after_death        rc=0
c10_death_before_receive     rc=0
c11_server_death             rc=0
c12_message_pi               rc=0
c13_mutex_pi_chain           rc=0
c14_rr_timeslice             rc=0
c15_channel_destroy_senders  rc=0
```

Elf inspection of a built binary: `ELF 64-bit LSB pie executable, x86-64`,
`interpreter /usr/lib/ldqnx-64.so.2`, `NEEDED libc.so.6`, `libgcc_s.so.1`.

This is scenario **self-checking** on the target, not trace comparison. The
scenarios assert their own outcomes; they do not prove that the simulator's
event sequence matches QNX's.

## 3. Three scenario bugs found by running on the target

All three compiled and passed on the simulator, and only failed or hung on real
QNX. They are fixed in `conformance/scenarios/`:

| Scenario | Bug | Why the simulator missed it | Fix |
|---|---|---|---|
| `c11_server_death` | `g_clients_failed` counts failures, but the assertion was `!= CLIENTS`, so it required every client to fail. On QNX all three correctly got `ESRCH`, so the counter stayed 0 and the test failed. | C11 is **skipped** on the simulator (server death needs process teardown), so the inverted assertion was never exercised. | Count successes as `g_clients_ok` and require `== CLIENTS`. |
| `c12_message_pi` | `SchedGet()` was treated as POSIX `sched_getparam()`: success compared to `0`. QNX returns the scheduling policy on success (`1` = `SCHED_FIFO`), so `g_seen` was never assigned. | The simulator's model returns 0, matching the POSIX assumption. | Compare `!= -1`. With the fix the observed priorities are `20` for an inheriting channel and `5` under `_NTO_CHF_FIXED_PRIORITY`, which is the behaviour the scenario intends. |
| `c13_mutex_pi_chain` | Deadlock by construction: `low_thread` held the mutex until `g_done`, which is set only after `pthread_join(high)`, and `high` waits for that mutex. | C13 is **skipped** until the mutex shim exists (LIMITS #37). | Hold the lock for a bounded 50 ms instead of waiting on `g_done`. |

The C12 result is the useful one: message-driven priority inheritance and the
`FIXED_PRIORITY` variant **do** behave as modelled — the scenario simply read
the return code with the wrong convention.

## 4. What is still missing, and why

`permute-capture` (PRD ADR-010, `libpermute_record.so` + a ~600-line agent)
was never implemented: there is no source file, no build target, and no commit
for it. Without it there is no on-target event stream, so
`conformance/oracle/qnx/` cannot be populated and the comparator cannot run
against a QNX reference.

There is also an unresolved design question before it can be written: the
committed simulator references contain scheduler-internal events (`C01` holds
2,001 `SCHED_PICK`, 1,999 `BLOCK`, 1,999 `UNBLOCK` for 1,000 messages), and
`conformance/ALLOWED-DIFFERENCES.md` does not permit a target capture to omit
them. A capture agent that only observes API calls will not reproduce that
sequence.

## 5. Procedure used

1. Install SDP 8.0 and activate the non-commercial licence:
   `tools/qnx_bootstrap.sh` (QNX Software Center 2.0.4, `-activateAll`).
2. Cross-build every scenario:
   `tools/qnx_build_scenarios.sh build/qnx` →
   [`build.log`](evidence/qnx/build.log).
3. Build a bootable image with the SDP's `mkqnximage` (type `qemu`, x86_64,
   `--cpu=4 --ram=2G`), adding the binaries through
   `local/snippets/system_files.custom` and a runner through
   `local/snippets/post_start.custom`.
4. Boot headless with the command in
   [`qemu.log`](evidence/qnx/qemu.log), serial captured to a file.
5. Record `uname -a`, the image hashes, and the 14 results.

## 6. Integrity rules

- Simulator output is never copied into `conformance/oracle/qnx/`.
- A capture is committed only with the manifest that identifies its SDP
  version and QEMU invocation.
- If a capture must be re-taken, the old one is replaced, not averaged.
- `make conformance` continues to label its oracle as the simulator until a
  manifest exists under `conformance/oracle/qnx/`.
