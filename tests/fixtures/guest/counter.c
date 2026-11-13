// SPDX-License-Identifier: Apache-2.0
//
// One guest process instance. Compiled twice, into two separate .so files,
// which is what gives each instance its own statics.
//
// Commands:
//   set <n>   store n in this instance's static, return it
//   get       return this instance's static
//   probe     call an interposed symbol; with no kernel table it must fail
//
// The constructor appends GUEST_INSTANCE to $PERMUTE_CTOR_LOG if set, which
// is how the loader test observes spawn order.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abi/permute_iface.h"

#ifndef GUEST_INSTANCE
#define GUEST_INSTANCE "0"
#endif

static int counter;

__attribute__((constructor)) static void guest_constructor(void)
{
    const char *log = getenv("PERMUTE_CTOR_LOG");
    if (log == NULL)
        return;

    FILE *f = fopen(log, "a");
    if (f == NULL)
        return;
    fprintf(f, "%s\n", GUEST_INSTANCE);
    fclose(f);
}

int permute_main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "set") == 0) {
        counter = atoi(argv[2]);
        return counter;
    }
    if (argc >= 2 && strcmp(argv[1], "get") == 0)
        return counter;
    if (argc >= 2 && strcmp(argv[1], "probe") == 0)
        return (int)MsgSend(-1, NULL, 0, NULL, 0);
    return -1;
}
