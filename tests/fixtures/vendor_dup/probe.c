// SPDX-License-Identifier: Apache-2.0
//
// Link probe for the duplicate-symbol gate.
//
// It references vendor_marker so the vendor object is pulled from its archive,
// and ChannelCreate so the shim object is pulled from ours. Both objects
// define MsgSend, so the link must fail.

#include "abi/permute_iface.h"

int vendor_marker(void);

int main(void)
{
    return vendor_marker() + (int)MsgSend(0, NULL, 0, NULL, 0) +
           ChannelCreate(0);
}
