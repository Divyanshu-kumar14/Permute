// SPDX-License-Identifier: Apache-2.0
//
// Mutation fixture for M07: the receive-time priority assignment must use the
// channel being received from, not the maximum over every channel.
#include "conformance/scenarios/common.h"

static int g_ch1 = -1;
static int g_ch2 = -1;
static volatile int g_failed;
static volatile int g_seen;

static void *client_thread(void *arg)
{
    int prio = *(int *)arg;
    c_set_prio(prio);
    int chid = (prio == 30) ? g_ch2 : g_ch1;
    int coid = ConnectAttach(0, 0, chid, 0, 0);
    if (coid < 0) { g_failed = 1; return NULL; }
    if (MsgSend(coid, "m", 1, NULL, 0) != 0) g_failed = 1;
    ConnectDetach(coid);
    return NULL;
}

static void *server_thread(void *arg)
{
    (void)arg;
    c_set_prio(5);
    c_sleep_ms(1); // both messages queued: 10 on ch1, 30 on ch2

    char msg[8];
    rcvid_t rv = MsgReceive(g_ch1, msg, sizeof msg, NULL);
    if (rv <= 0) { g_failed = 1; return NULL; }
    struct sched_param param;
    memset(&param, 0, sizeof param);
    if (SchedGet(0, 0, &param) == 0)
        g_seen = param.sched_priority;
    if (MsgReply(rv, 0, NULL, 0) != 0) g_failed = 1;

    rv = MsgReceive(g_ch2, msg, sizeof msg, NULL);
    if (rv <= 0) { g_failed = 1; return NULL; }
    if (MsgReply(rv, 0, NULL, 0) != 0) g_failed = 1;
    return NULL;
}

int permute_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    g_ch1 = ChannelCreate(0);
    g_ch2 = ChannelCreate(0);
    if (g_ch1 < 0 || g_ch2 < 0) return 1;

    pthread_t server;
    if (pthread_create(&server, NULL, server_thread, NULL) != 0) return 1;

    int prios[2] = { 10, 30 };
    pthread_t clients[2];
    for (int i = 0; i < 2; i++)
        if (pthread_create(&clients[i], NULL, client_thread, &prios[i]) != 0)
            return 1;
    for (int i = 0; i < 2; i++) pthread_join(clients[i], NULL);
    pthread_join(server, NULL);

    // The receive on ch1 must leave the server at the 10 of that message, not
    // the 30 queued on ch2.
    if (g_seen != 10) g_failed = 1;

    ChannelDestroy(g_ch1);
    ChannelDestroy(g_ch2);
    return g_failed ? 1 : 0;
}

#ifdef __QNXNTO__
int main(int argc, char **argv) { return permute_main(argc, argv); }
#endif
