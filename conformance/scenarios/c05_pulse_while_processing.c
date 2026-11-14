// SPDX-License-Identifier: Apache-2.0
// C05 — a pulse sent while the server is processing is queued and taken next.
#include "conformance/scenarios/common.h"

static int g_chid = -1;
static int g_coid = -1;
static volatile int g_failed;
static volatile int g_processing;

static void *client_thread(void *arg)
{
    (void)arg;
    c_set_prio(10);
    if (MsgSend(g_coid, "a", 1, NULL, 0) != 0)
        g_failed = 1;
    return NULL;
}

static void *server_thread(void *arg)
{
    (void)arg;
    c_set_prio(20);

    char msg[8];
    rcvid_t rv = MsgReceive(g_chid, msg, sizeof msg, NULL);
    if (rv <= 0) { g_failed = 1; return NULL; }

    // Processing: the pulse arrives during this window.
    g_processing = 1;
    c_sleep_ms(1);
    if (MsgReply(rv, 0, NULL, 0) != 0) g_failed = 1;

    struct _pulse pulse;
    memset(&pulse, 0, sizeof pulse);
    if (MsgReceivePulse(g_chid, &pulse, sizeof pulse, NULL) != 0)
        g_failed = 1;
    if (pulse.code != 0x45 || pulse.value.sival_int != 9)
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

    // Wait until the server holds the message, then pulse while it processes.
    while (!g_processing)
        SchedYield();
    if (MsgSendPulse(g_coid, 30, 0x45, 9) != 0) g_failed = 1;

    pthread_join(client, NULL);
    pthread_join(server, NULL);
    ConnectDetach(g_coid);
    ChannelDestroy(g_chid);
    return g_failed ? 1 : 0;
}

#ifdef __QNXNTO__
int main(int argc, char **argv) { return permute_main(argc, argv); }
#endif
