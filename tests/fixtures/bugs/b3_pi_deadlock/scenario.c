// SPDX-License-Identifier: Apache-2.0
//
// B3 — priority inversion deadlock: the server drops its own priority while
// holding a client's request, then waits on a mid-priority helper.
//
// H (20) waits for S. S, after lowering itself to 5, waits for M (15). M, at
// the priority the boosted server should have lent it, waits on H's channel.
// H is reply-blocked, so M's send can never be received: all three are
// blocked. The defect is the SchedSet down to 5 while a client is waiting.
//
// One seed in four takes the losing path; the rest reply directly.

#include <pthread.h>
#include <sched.h>
#include <string.h>

#include "abi/permute_iface.h"

static int g_srv_chid = -1;
static int g_mid_chid = -1;
static int g_h_chid = -1;
static int g_scoid = -1;
static int g_mid_coid = -1;
static volatile int g_failed;
static int g_race;

static void set_prio(int priority)
{
    struct sched_param param;
    memset(&param, 0, sizeof param);
    param.sched_priority = priority;
    if (SchedSet(0, 0, SCHED_FIFO, &param) != 0)
        g_failed = 1;
}

static void *helper_thread(void *arg)
{
    (void)arg;
    set_prio(20);

    // H's own channel is where a helper would report, but H is reply-blocked
    // by the time M sends; the message queues and M blocks in SEND.
    int coid = ConnectAttach(0, 0, g_srv_chid, 0, 0);
    if (coid < 0) {
        g_failed = 1;
        return NULL;
    }

    unsigned char msg[16];
    unsigned char reply[16];
    if (MsgSend(coid, "h", 2, reply, sizeof reply) != 0)
        g_failed = 1;
    ConnectDetach(coid);
    return NULL;
}

static void *mid_thread(void *arg)
{
    (void)arg;
    set_prio(15);

    struct sigevent unblock;
    memset(&unblock, 0, sizeof unblock);
    SIGEV_UNBLOCK_INIT(&unblock);

    uint64_t ntime = 5ull * 1000ull * 1000ull;
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, &unblock, &ntime,
                     NULL) != 0) {
        g_failed = 1;
        return NULL;
    }

    unsigned char msg[16];
    struct _msg_info info;
    rcvid_t rv = MsgReceive(g_mid_chid, msg, sizeof msg, &info);
    if (rv < 0)
        return NULL; // no work arrived: the fixed server replied directly

    // S waits for this send. H cannot receive it, so this never completes.
    unsigned char reply[16];
    (void)MsgSend(g_h_chid, "m", 2, reply, sizeof reply);
    return NULL;
}

static void *server_thread(void *arg)
{
    (void)arg;
    set_prio(10);

    unsigned char msg[16];
    struct _msg_info info;
    rcvid_t rv = MsgReceive(g_srv_chid, msg, sizeof msg, &info);
    if (rv <= 0) {
        g_failed = 1;
        return NULL;
    }

    if (g_race) {
        // Defect: the serving thread lowers itself while a client waits, then
        // blocks on a helper. The client's priority never reaches the helper.
        set_prio(5);
        unsigned char buf[16];
        if (MsgSend(g_mid_coid, "s", 2, buf, sizeof buf) != 0)
            g_failed = 1;
    }

    (void)MsgReply(rv, 0, msg, info.msglen);
    return NULL;
}

int permute_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    g_race = (rand() % 4) == 0;

    g_srv_chid = ChannelCreate(0);
    g_mid_chid = ChannelCreate(0);
    if (g_srv_chid < 0 || g_mid_chid < 0)
        return 1;

    g_h_chid = g_srv_chid; // H sends to the server; its channel is unused
    g_scoid = ConnectAttach(0, 0, g_srv_chid, 0, 0);
    g_mid_coid = ConnectAttach(0, 0, g_mid_chid, 0, 0);
    if (g_scoid < 0 || g_mid_coid < 0)
        return 1;

    pthread_t server, mid, helper;
    if (pthread_create(&server, NULL, server_thread, NULL) != 0)
        return 1;
    if (pthread_create(&mid, NULL, mid_thread, NULL) != 0)
        return 1;
    if (pthread_create(&helper, NULL, helper_thread, NULL) != 0)
        return 1;

    (void)pthread_join(helper, NULL);
    (void)pthread_join(server, NULL);
    (void)pthread_join(mid, NULL);

    ConnectDetach(g_scoid);
    ConnectDetach(g_mid_coid);
    ChannelDestroy(g_mid_chid);
    ChannelDestroy(g_srv_chid);
    return g_failed ? 1 : 0;
}
