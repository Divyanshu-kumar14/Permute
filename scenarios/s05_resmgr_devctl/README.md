<!-- SPDX-License-Identifier: Apache-2.0 -->

# s05_resmgr_devctl — Tier 2 kill-criterion probe

The probe evaluated the canonical "Bones of a Resource Manager" example from
the QNX 8.0 Programmer's Guide. The vendor sample itself is **not bundled**:
it is QNX sample code, and this repository ships no third-party source.
`client.c` is the original client half — it opens `/dev/hello` and issues a
`devctl` — kept because the ABI gap it demonstrates is still the evidence.

The P2-11 evaluation asked whether the sample runs under the simulator. It does
not: the clean-room ABI has no `<sys/dispatch.h>` or `<sys/iofunc.h>`, and the
kernel/runtime export no `dispatch_*`, `resmgr_attach`, `iofunc_*`, pathname
namespace, or `name_attach`. The build transcript is in
`gate/p2-11-tier2-evaluation.md`; the decision is in `docs/DECISIONS.md` §4.1.

Nothing here is named `scenario.c`, so `make scenarios` does not attempt to
build it before the API exists.
