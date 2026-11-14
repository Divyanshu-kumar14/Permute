// SPDX-License-Identifier: Apache-2.0
// C04 — a pulse and a message are both delivered to a blocked MsgReceive.
#include "conformance/scenarios/common.h"

static int g_chid = -1;
static int g_coid = -1;
static volatile int g_failed;

static void *client_thread(void *arg)
{
    (void)arg;
    c_set_prio(10);
    if (MsgSend(g_coid, "m", 1, NULL, 0) != 0)
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

    // The pulse is higher priority, so a blocked receive takes it first.
    if (MsgSendPulse(g_coid, 30, 0x44, 7) != 0) g_failed = 1;

    pthread_t client;
    if (pthread_create(&client, NULL, client_thread, NULL) != 0) return 1;

    c_sleep_ms(1); // both the pulse and the message are now queued

    struct _pulse pulse;
    memset(&pulse, 0, sizeof pulse);
    rcvid_t rv = MsgReceive(g_chid, &pulse, sizeof pulse, NULL);
    if (rv != 0 || pulse.code != 0x44 || pulse.value.sival_int != 7)
        g_failed = 1;

    char msg[8];
    rv = MsgReceive(g_chid, msg, sizeof msg, NULL);
    if (rv <= 0 || msg[0] != 'm') g_failed = 1;
    else if (MsgReply(rv, 0, NULL, 0) != 0) g_failed = 1;

    pthread_join(client, NULL);
    ConnectDetach(g_coid);
    ChannelDestroy(g_chid);
    return g_failed ? 1 : 0;
}

#ifdef __QNXNTO__
int main(int argc, char **argv) { return permute_main(argc, argv); }
#endif
