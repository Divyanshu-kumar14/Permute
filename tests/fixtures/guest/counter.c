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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

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
    if (argc >= 2 && strcmp(argv[1], "probe") == 0) {
        // Reference every interposed symbol so the packaging test can prove
        // the whole surface is exported by the guest object.
        //
        // With no kernel table installed, each call must fail. The clock
        // results are checked explicitly: if the object's own definition were
        // preempted by libc, these would succeed and the guest would silently
        // read the host clock.
        struct timespec ts;
        struct timeval tv;
        memset(&ts, 0, sizeof ts);
        memset(&tv, 0, sizeof tv);

        static volatile int sink;
        sink += (int)getpid();
        sink += (int)gettid();
        sink += pthread_equal(pthread_self(), (pthread_t)0);

        int clock_ok = 0;
        clock_ok += (clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
        clock_ok += (time(NULL) != (time_t)-1);
        clock_ok += (gettimeofday(&tv, NULL) == 0);
        clock_ok += (nanosleep(&ts, NULL) == 0);
        if (clock_ok != 0)
            return 100 + clock_ok; // a real clock was reachable

        // Entropy must be the shim's, not libc's: the same seed must replay.
        srand(5);
        int r1 = rand();
        srand(5);
        int r2 = rand();
        (void)random();
        if (r1 != r2)
            return 150;

        (void)ChannelCreate(0);
        (void)ConnectAttach(0, 0, 0, 0, 0);
        (void)MsgReceive(0, NULL, 0, NULL);
        (void)MsgReply(0, 0, NULL, 0);
        (void)sink;
        return (int)MsgSend(-1, NULL, 0, NULL, 0);
    }
    return -1;
}
