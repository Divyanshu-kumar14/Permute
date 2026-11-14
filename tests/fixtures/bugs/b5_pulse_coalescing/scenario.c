// SPDX-License-Identifier: Apache-2.0
//
// B5 — pulse coalescing assumption: the consumer treats one pulse as exactly
// one event and stops reading.
//
// Two events arrive close together and the producer sends one pulse each. The
// buggy consumer drains a single pulse and asserts the channel is empty; the
// second pulse is still queued, so the assertion fails. The fix drains until
// the queue is empty.
//
// Half the seeds produce two events, half produce one.

#include <pthread.h>
#include <sched.h>
#include <string.h>

#include "abi/permute_iface.h"

static int g_chid = -1;
static int g_coid = -1;
static volatile int g_failed;
static int g_events;

static int recv_one(uint64_t timeout_ns)
{
    struct sigevent unblock;
    memset(&unblock, 0, sizeof unblock);
    SIGEV_UNBLOCK_INIT(&unblock);

    uint64_t ntime = timeout_ns;
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, &unblock, &ntime,
                     NULL) != 0)
        return -2;

    struct _pulse pulse;
    memset(&pulse, 0, sizeof pulse);
    if (MsgReceivePulse(g_chid, &pulse, sizeof pulse, NULL) != 0)
        return -1;
    return 0;
}

int permute_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    g_events = 1 + (rand() % 2);

    g_chid = ChannelCreate(0);
    if (g_chid < 0)
        return 1;
    g_coid = ConnectAttach(0, 0, g_chid, 0, 0);
    if (g_coid < 0)
        return 1;

    for (int i = 0; i < g_events; i++) {
        if (MsgSendPulse(g_coid, -1, 0x60, i) != 0)
            g_failed = 1;
    }

    // The consumer takes one pulse per event, on the assumption that the
    // producer coalesced its notifications.
    if (recv_one(5ull * 1000 * 1000) != 0)
        g_failed = 1;

    // Defect: the channel is assumed to be empty now.
    if (recv_one(1000 * 1000) != -1)
        g_failed = 1;

    ConnectDetach(g_coid);
    ChannelDestroy(g_chid);
    return g_failed ? 1 : 0;
}
