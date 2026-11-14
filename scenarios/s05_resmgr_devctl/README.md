<!-- SPDX-License-Identifier: Apache-2.0 -->

# s05_resmgr_devctl — Tier 2 kill-criterion probe

`skeleton.c` is the canonical "Bones of a Resource Manager" example from the
QNX 8.0 Programmer's Guide, unmodified. `client.c` opens `/dev/hello` and
issues a `devctl`.

The P2-11 evaluation asks whether this runs under the simulator. It does not:
the clean-room ABI has no `<sys/dispatch.h>` or `<sys/iofunc.h>`, and the
kernel/runtime export no `dispatch_*`, `resmgr_attach`, `iofunc_*`, pathname
namespace, or `name_attach`. The exact build output is in
`gate/p2-11-tier2-evaluation.md`.

The file is not named `scenario.c` so `make scenarios` does not attempt to
build it before the API exists.
