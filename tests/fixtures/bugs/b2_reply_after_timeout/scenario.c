// SPDX-License-Identifier: Apache-2.0
//
// B2 — reply after timeout: the server answers a request whose rcvid the
// client has already abandoned.
//
// One seed in four gives the client a 1 ms deadline; the buggy server sleeps
// 2 ms before replying, so the client times out and the reply is orphaned.
// The other seeds use a 5 ms deadline and pass. The fix is to answer promptly.

#include <pthread.h>
#include <sched.h>
#include <string.h>

#include "abi/permute_iface.h"

static int g_chid = -1;
static int g_coid = -1;
static volatile int g_failed;
static int g_race;
static int g_slow = 1; // cleared by the fixed build

static void set_prio(int priority)
{
    struct sched_param param;
    memset(&param, 0, sizeof param);
    param.sched_priority = priority;
    if (SchedSet(0, 0, SCHED_FIFO, &param) != 0)
        g_failed = 1;
}

static void *server_thread(void *arg)
{
    (void)arg;
    set_prio(12);

    unsigned char msg[16];
    struct _msg_info info;
    rcvid_t rv = MsgReceive(g_chid, msg, sizeof msg, &info);
    if (rv <= 0) {
        g_failed = 1;
        return NULL;
    }

    if (g_slow) {
        // Defect: the reply is produced after the client's deadline.
        struct timespec nap = { 0, 2 * 1000 * 1000 };
        (void)nanosleep(&nap, NULL);
    }

    // A stale rcvid fails with ESRCH; the client has already given up.
    (void)MsgReply(rv, 0, msg, info.msglen);
    return NULL;
}

int permute_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    g_race = (rand() % 4) == 0;

    g_chid = ChannelCreate(0);
    if (g_chid < 0)
        return 1;
    g_coid = ConnectAttach(0, 0, g_chid, 0, 0);
    if (g_coid < 0)
        return 1;

    pthread_t server;
    if (pthread_create(&server, NULL, server_thread, NULL) != 0)
        return 1;

    // Let the server reach MsgReceive first, so the send blocks in REPLY and
    // the reply-timeout mask applies from the start. Without this the send
    // blocks in SEND, which the REPLY bit does not cover.
    struct timespec settle = { 0, 1000 * 1000 };
    (void)nanosleep(&settle, NULL);

    struct sigevent unblock;
    memset(&unblock, 0, sizeof unblock);
    SIGEV_UNBLOCK_INIT(&unblock);

    uint64_t ntime = (g_race ? 1ull : 5ull) * 1000ull * 1000ull;
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_REPLY, &unblock, &ntime,
                     NULL) != 0)
        g_failed = 1;

    if (MsgSend(g_coid, "x", 1, NULL, 0) != 0)
        g_failed = 1; // the deadline was missed

    (void)pthread_join(server, NULL);
    ConnectDetach(g_coid);
    ChannelDestroy(g_chid);
    return g_failed ? 1 : 0;
}
