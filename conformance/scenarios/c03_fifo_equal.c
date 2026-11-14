// SPDX-License-Identifier: Apache-2.0
// C03 — FIFO within equal priority: three clients at 10. Exact.
#include "conformance/scenarios/common.h"

static int g_chid = -1;
static volatile int g_failed;
static volatile int g_order[3];
static volatile int g_seen;

typedef struct { int id; } payload_t;

static void *client_thread(void *arg)
{
    int id = *(int *)arg;
    c_set_prio(10);
    int coid = ConnectAttach(0, 0, g_chid, 0, 0);
    if (coid < 0) { g_failed = 1; return NULL; }
    payload_t msg = { id };
    if (MsgSend(coid, &msg, sizeof msg, NULL, 0) != 0)
        g_failed = 1;
    ConnectDetach(coid);
    return NULL;
}

static void *server_thread(void *arg)
{
    (void)arg;
    c_set_prio(30);
    c_sleep_ms(1);
    for (int i = 0; i < 3; i++) {
        payload_t msg;
        rcvid_t rv = MsgReceive(g_chid, &msg, sizeof msg, NULL);
        if (rv <= 0) { g_failed = 1; return NULL; }
        g_order[i] = msg.id;
        g_seen = i + 1;
        if (MsgReply(rv, 0, NULL, 0) != 0) g_failed = 1;
    }
    return NULL;
}

int permute_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    g_chid = ChannelCreate(0);
    if (g_chid < 0) return 1;
    pthread_t server;
    if (pthread_create(&server, NULL, server_thread, NULL) != 0) return 1;

    int ids[3] = { 1, 2, 3 };
    pthread_t clients[3];
    for (int i = 0; i < 3; i++)
        if (pthread_create(&clients[i], NULL, client_thread, &ids[i]) != 0)
            return 1;
    for (int i = 0; i < 3; i++) pthread_join(clients[i], NULL);
    pthread_join(server, NULL);

    if (g_seen != 3) g_failed = 1;
    if (g_order[0] != 1 || g_order[1] != 2 || g_order[2] != 3)
        g_failed = 1;

    ChannelDestroy(g_chid);
    return g_failed ? 1 : 0;
}

#ifdef __QNXNTO__
int main(int argc, char **argv) { return permute_main(argc, argv); }
#endif
