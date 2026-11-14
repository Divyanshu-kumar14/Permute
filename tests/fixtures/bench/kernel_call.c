// SPDX-License-Identifier: Apache-2.0
//
// NFR-3 probe: the cost of a non-blocking kernel query through the shim path.
#include <sched.h>
#include <string.h>

#include "abi/permute_iface.h"

#define CALL_COUNT 2000000

int permute_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    struct sched_param param;
    memset(&param, 0, sizeof param);
    for (int i = 0; i < CALL_COUNT; i++)
        (void)SchedGet(0, 0, &param);
    return 0;
}
