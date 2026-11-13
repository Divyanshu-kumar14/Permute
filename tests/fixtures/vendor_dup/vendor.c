// SPDX-License-Identifier: Apache-2.0
//
// A vendor archive that already defines an interposed symbol. This is the
// situation FR-2.6 exists for: the build must fail and name both objects,
// rather than silently picking one definition.

#include <stddef.h>

#include "abi/permute_abi.h"

int vendor_marker(void)
{
    return 7;
}

long MsgSend(int coid, const void *smsg, size_t sbytes, void *rmsg,
             size_t rbytes)
{
    (void)coid;
    (void)smsg;
    (void)sbytes;
    (void)rmsg;
    (void)rbytes;
    return 0;
}
