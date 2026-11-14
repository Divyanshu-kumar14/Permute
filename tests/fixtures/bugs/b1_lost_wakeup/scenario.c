// SPDX-License-Identifier: Apache-2.0
//
// B1 — lost wakeup: the notification is armed after the request is sent.
//
// The client asks the server to deliver a pulse event. The event must be
// registered with the server connection before the server holds the message;
// arming it afterwards loses the wakeup and the client waits forever.
//
// One seed in eight takes the losing order, so this is a race, not a
// deterministic failure: the other seeds arm first and pass.

#include <pthread.h>
#include <sched.h>
#include <string.h>

#include "abi/permute_iface.h"

static int g_srv_chid = -1;
static int g_client_chid = -1;
static int g_scoid = -1;
static int g_self_coid = -1;
static struct sigevent g_event;
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

static void *server_thread(void *arg)
{
    (void)arg;
    set_prio(12);

    unsigned char msg[16];
    struct _msg_info info;
    rcvid_t rv = MsgReceive(g_srv_chid, msg, sizeof msg, &info);
    if (rv <= 0) {
        g_failed = 1;
        return NULL;
    }

    // Delivers only when the client armed the event before this receive.
    (void)MsgDeliverEvent(rv, &g_event);
    (void)MsgReply(rv, 0, msg, info.msglen);
    return NULL;
}

int permute_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    int seed = rand();
    g_race = (seed % 8) == 0;

    g_srv_chid = ChannelCreate(0);
    g_client_chid = ChannelCreate(0);
    if (g_srv_chid < 0 || g_client_chid < 0)
        return 1;

    g_scoid = ConnectAttach(0, 0, g_srv_chid, 0, 0);
    g_self_coid = ConnectAttach(0, 0, g_client_chid, 0, 0);
    if (g_scoid < 0 || g_self_coid < 0)
        return 1;

    memset(&g_event, 0, sizeof g_event);
    SIGEV_PULSE_INIT(&g_event, g_self_coid, 10, 0x51, 0);

    pthread_t server;
    if (pthread_create(&server, NULL, server_thread, NULL) != 0)
        return 1;

    if (g_race) {
        // Defect: send first, arm the notification only after the reply.
        (void)MsgSend(g_scoid, "x", 1, NULL, 0);
        (void)MsgRegisterEvent(&g_event, g_scoid);
    } else {
        if (MsgRegisterEvent(&g_event, g_scoid) != 0)
            g_failed = 1;
        (void)MsgSend(g_scoid, "x", 1, NULL, 0);
    }

    // The wakeup must have arrived by now. A timeout means it was lost.
    struct sigevent unblock;
    memset(&unblock, 0, sizeof unblock);
    SIGEV_UNBLOCK_INIT(&unblock);

    uint64_t ntime = 5ull * 1000ull * 1000ull;
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, &unblock, &ntime,
                     NULL) != 0)
        g_failed = 1;

    struct _pulse pulse;
    memset(&pulse, 0, sizeof pulse);
    if (MsgReceivePulse(g_client_chid, &pulse, sizeof pulse, NULL) != 0)
        g_failed = 1;

    (void)pthread_join(server, NULL);
    ConnectDetach(g_self_coid);
    ConnectDetach(g_scoid);
    ChannelDestroy(g_client_chid);
    ChannelDestroy(g_srv_chid);
    return g_failed ? 1 : 0;
}
