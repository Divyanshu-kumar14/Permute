// SPDX-License-Identifier: Apache-2.0
// C10 — a client cancelled while SEND-blocked leaves no message behind.
#include "conformance/scenarios/common.h"

static int g_chid = -1;
static volatile int g_failed;
static volatile int g_send_blocked;

static void *client_thread(void *arg)
{
    (void)arg;
    if (MsgSend(g_chid, "x", 1, NULL, 0) != 0)
        g_send_blocked = 1;
    return NULL;
}

int permute_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    g_chid = ChannelCreate(0);
    if (g_chid < 0) return 1;

    pthread_t client;
    if (pthread_create(&client, NULL, client_thread, NULL) != 0) return 1;

    c_sleep_ms(1); // the client is SEND-blocked with no receiver
    if (pthread_cancel(client) != 0) g_failed = 1;
    pthread_join(client, NULL);

    // The cancelled send's message must be gone, not delivered to us.
    if (c_arm(_NTO_TIMEOUT_RECEIVE, C_MS(1)) != 0) g_failed = 1;
    char msg[8];
    if (MsgReceive(g_chid, msg, sizeof msg, NULL) != -1 ||
        errno != ETIMEDOUT)
        g_failed = 1;

    ChannelDestroy(g_chid);
    return g_failed ? 1 : 0;
}

#ifdef __QNXNTO__
int main(int argc, char **argv) { return permute_main(argc, argv); }
#endif
