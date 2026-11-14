<!-- SPDX-License-Identifier: Apache-2.0 -->

# Security policy

**Owner:** [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14)
**Status:** current for v0.1 (pre-release).

---

## Supported versions

| Version | Supported |
|---|---|
| `main` (pre-release) | yes |
| tagged `v0.1.x` | security fixes on the latest tag |
| older tags | no |

Permute is a developer tool, not a service. There is no hosted component, no
account system, and no network listener.

## Reporting a vulnerability

Use GitHub's private vulnerability reporting on this repository
(“Security” → “Report a vulnerability”). Do not open a public issue for a
security problem.

Include the version or commit, the command, the observed behaviour, and a
minimal reproducer if one exists. We aim to acknowledge within 7 days and to
ship a fix or a documented mitigation within 90 days.

## What is in scope

| Area | Examples |
|---|---|
| Harness memory safety | out-of-bounds read/write, use-after-free, double free in the kernel, shim, codec, or allocator |
| Deterministic allocator | arena boundary escape, size-class corruption, ownership confusion with libc pointers |
| Trace parser and replay | malformed `.pmt` causing a crash or unbounded allocation; digest bypass |
| Report generation | HTML/JSON injection from a scenario name or payload; path traversal in report output |
| CLI | unsafe path handling in `init`, `build`, `shrink`, `clean`; config parser crash |
| Crash isolation | a SUT crash escaping the worker and taking down the parent |

`make asan` is the expected first check for a memory-safety report.

## What is out of scope

- **Bugs in the subject under test.** Permute finds them; they are not
  vulnerabilities in Permute.
- **The documented fidelity gaps.** Signals, files, mutexes, resource managers
  and target-binary execution are not modelled. `LIMITS.md` says so; a report
  that they behave differently is a known limit, not a vulnerability.
- **QNX or third-party code.** No vendor code is distributed with this
  repository.
- **Denial of service by design.** A scenario is arbitrary code the user chose
  to run.

## Important: this is not a sandbox

A guest object is `dlopen`ed into the host process. It runs with the host user's
privileges and can call any libc function that resolves, including file and
network APIs. Permute isolates crashes between seeds with a process boundary; it
does **not** isolate the SUT from the machine.

Do not run an untrusted scenario. The safe pattern is a container or VM with the
repository mounted read-only and no credentials available.

## Secrets

No secrets, tokens, or credentials belong in the repository, a scenario, a
config, or a trace. The worker does not read the environment of the SUT beyond
the variables it sets explicitly. If you find a committed secret, report it
privately; assume it is compromised and rotate it.
