<!-- SPDX-License-Identifier: Apache-2.0 -->

# Contributing

**Owner:** [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14)
**Status:** current for v0.1.

Permute is pre-release. The design is settled enough that a change which
contradicts an accepted decision will not merge without a new decision that
supersedes it.

---

## Before you write code

1. Read [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for how the model works.
2. Read [`docs/DECISIONS.md`](docs/DECISIONS.md) for why. ADR-001..ADR-011 are
   architectural; IMP-1..IMP-92 are implementation choices with reasons.
3. Check [`docs/LIMITS.md`](docs/LIMITS.md). “It should work” is not a claim
   this project makes; if the model does not cover it, say so.

A new architectural decision goes in `DECISIONS.md` with context, options,
decision, and consequence. A new limit goes in `LIMITS.md` with a number. A
reader-facing document needs `**Owner:**` and `**Status:**`; `make docs-check`
enforces that.

## Build and test

```sh
make build        # CMake + Ninja
make test         # unit, integration, scenarios, gates
make ci           # the merge gate (test + docs + quickstart + licence + determinism + coverage + CI check)
make gate1        # G1 scorecard: re-runs every criterion and writes gate/gate1-scorecard.md
make asan         # AddressSanitizer + UBSan build and suite (required for C changes)
```

`make ci` must be green before a change is proposed. If you touched C, run
`make asan` too; a sanitizer finding is a bug, not a warning. If you touched a
scored area, `make gate1` must stay green and the scorecard is regenerated.

## Code rules

| Rule | Why |
|---|---|
| C11, no compiler extensions beyond the ones already in use | the QNX toolchain is the other target |
| No host clock, entropy, identity, or unordered map in production C | the build gate fails on them; route through the kernel model |
| The shim owns no state and retains no payload pointer | a retained SUT pointer is a use-after-free and a determinism leak |
| The kernel never picks the next thread | see `docs/KERNEL-CONTRACT.md` §1 |
| Fail loudly on an unsupported call | a silent approximation becomes a false conformance result |
| Every interposed symbol is declared once in `abi/permute_iface.def` | the audit is generated from it; see `docs/ABI.md` §7 |
| SPDX header on every new file | Apache-2.0, as in `LICENSE` |

No vendor headers, SDP files, binaries, QNX logos, or secrets may be committed.
The clean-room declarations in `abi/` are written from public documentation
(ADR-011).

## Tests

- A behaviour change needs a test that fails before and passes after.
- Cover the normal, empty, and invalid case. “Invalid” means a stale handle, a
  destroyed channel, a short buffer, or an unarmed fault — whichever applies.
- A conformance-visible change needs a scenario, not only a unit test.
- A determinism-visible change must keep `make determinism` at 1,000/1,000 and
  `make verify` green.
- Do not weaken a gate to make a change pass. If a threshold is wrong, change it
  with a recorded decision and say why.

## Commits and pull requests

Conventional commits (`feat:`, `fix:`, `docs:`, `test:`, `refactor:`, `chore:`).
The body should say what changed, why, and which FR or task it serves. Keep a
task's changes in one commit where practical.

A pull request states:

1. the behaviour before and after;
2. the command that proves it (`make ci`, a suite line, a gate row);
3. any new limit or decision, with its ID;
4. any check that could not be run, and why.

## Reporting a problem

- A bug in the harness: open an issue with the seed, the config, the command,
  and the trace digest.
- A security issue: follow [`SECURITY.md`](SECURITY.md), not a public issue.
- A fidelity gap: include the scenario source and what QNX does differently.
  Without a target capture it is a hypothesis, and `LIMITS.md` is where it
  belongs.
