// SPDX-License-Identifier: Apache-2.0
//
// Declarations for the interposed surface, generated from the single source of
// truth. Include this in anything that calls an interposed symbol; never
// re-declare one by hand.

#ifndef PERMUTE_IFACE_H
#define PERMUTE_IFACE_H

#include "abi/permute_abi.h"

// musl defines pthread_equal as a function-like macro, which would expand
// inside the generated prototype. The interposed surface needs the name.
#ifdef pthread_equal
#undef pthread_equal
#endif

#define PERMUTE_SYMBOL(group, ret, name, params, flags) ret name params;
#include "abi/permute_iface.def"
#undef PERMUTE_SYMBOL

#endif // PERMUTE_IFACE_H
