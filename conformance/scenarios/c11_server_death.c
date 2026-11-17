// SPDX-License-Identifier: Apache-2.0
// C11 — destroying the channel unblocks reply-blocked clients with ESRCH.
#include "conformance/scenarios/common.h"

#define CLIENTS 3

static int g_chid = -1;
static volatile int g_failed;
static volatile int g_received;
static volatile int g_clients_ok;

static void *client_thread(void *arg)
{
    (void)arg;
    int coid = ConnectAttach(0, 0, g_chid, 0, 0);
    if (coid < 0) { g_failed = 1; return NULL; }
    if (MsgSend(coid, "c", 1, NULL, 0) == -1 && errno == ESRCH)
        g_clients_ok++;
    ConnectDetach(coid);
    return NULL;
}

static void *server_thread(void *arg)
{
    (void)arg;
    c_set_prio(20);
    rcvid_t held[CLIENTS];
    for (int i = 0; i < CLIENTS; i++) {
        held[i] = MsgReceive(g_chid, NULL, 0, NULL);
        if (held[i] <= 0) { g_failed = 1; return NULL; }
        g_received = i + 1;
    }

    // Hold the replies until the channel is gone.
    c_sleep_ms(2);
    for (int i = 0; i < CLIENTS; i++) {
        if (MsgReply(held[i], 0, NULL, 0) != -1 || errno != ESRCH)
            g_failed = 1;
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

    pthread_t clients[CLIENTS];
    for (int i = 0; i < CLIENTS; i++)
        if (pthread_create(&clients[i], NULL, client_thread, NULL) != 0)
            return 1;

    // Wait until every client is reply-blocked, then kill the channel.
    while (g_received < CLIENTS)
        SchedYield();
    if (ChannelDestroy(g_chid) != 0) g_failed = 1;

    for (int i = 0; i < CLIENTS; i++) pthread_join(clients[i], NULL);
    pthread_join(server, NULL);

    if (g_clients_ok != CLIENTS) g_failed = 1;
    return g_failed ? 1 : 0;
}

#ifdef __QNXNTO__
int main(int argc, char **argv) { return permute_main(argc, argv); }
#endif
