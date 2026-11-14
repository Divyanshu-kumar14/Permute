// SPDX-License-Identifier: Apache-2.0
// C09 — reply after the client has timed out and exited: ESRCH.
#include "conformance/scenarios/common.h"

static int g_chid = -1;
static int g_coid = -1;
static volatile int g_failed;

static void *client_thread(void *arg)
{
    (void)arg;
    if (c_arm(_NTO_TIMEOUT_REPLY, C_MS(1)) != 0) g_failed = 1;
    if (MsgSend(g_coid, "d", 1, NULL, 0) != -1 || errno != ETIMEDOUT)
        g_failed = 1;
    return NULL;
}

static void *server_thread(void *arg)
{
    (void)arg;
    rcvid_t rv = MsgReceive(g_chid, NULL, 0, NULL);
    if (rv <= 0) { g_failed = 1; return NULL; }
    c_sleep_ms(3);
    if (MsgReply(rv, 0, NULL, 0) != -1 || errno != ESRCH)
        g_failed = 1;
    return NULL;
}

int permute_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    g_chid = ChannelCreate(0);
    if (g_chid < 0) return 1;
    g_coid = ConnectAttach(0, 0, g_chid, 0, 0);
    if (g_coid < 0) return 1;

    pthread_t server, client;
    if (pthread_create(&server, NULL, server_thread, NULL) != 0) return 1;
    if (pthread_create(&client, NULL, client_thread, NULL) != 0) return 1;
    pthread_join(client, NULL);
    pthread_join(server, NULL);

    ConnectDetach(g_coid);
    ChannelDestroy(g_chid);
    return g_failed ? 1 : 0;
}

#ifdef __QNXNTO__
int main(int argc, char **argv) { return permute_main(argc, argv); }
#endif
