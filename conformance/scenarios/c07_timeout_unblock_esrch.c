// SPDX-License-Identifier: Apache-2.0
// C07 — client timeout, _NTO_CHF_UNBLOCK, then a late reply that finds the
// rcvid gone (ESRCH).
#include "conformance/scenarios/common.h"

static int g_failed;
static int g_coid;
static volatile int g_timed_out;

static void *unblock_client(void *arg)
{
    (void)arg;
    if (c_arm(_NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, C_MS(1)) != 0)
        g_failed = 1;
    if (MsgSend(g_coid, "u", 1, NULL, 0) != 0)
        g_failed = 1;
    return NULL;
}

static void *esrch_client(void *arg)
{
    (void)arg;
    if (c_arm(_NTO_TIMEOUT_REPLY, C_MS(1)) != 0)
        g_failed = 1;
    if (MsgSend(g_coid, "e", 1, NULL, 0) != 0)
        g_timed_out = 1;
    return NULL;
}

/* Phase 1: the unblock pulse keeps the client in REPLY; the reply succeeds. */
static void phase_unblock(void)
{
    int chid = ChannelCreate(_NTO_CHF_UNBLOCK);
    if (chid < 0) { g_failed = 1; return; }
    g_coid = ConnectAttach(0, 0, chid, 0, 0);
    if (g_coid < 0) { g_failed = 1; return; }

    pthread_t client;
    if (pthread_create(&client, NULL, unblock_client, NULL) != 0) {
        g_failed = 1;
        return;
    }

    rcvid_t rv = MsgReceive(chid, NULL, 0, NULL);
    if (rv <= 0) g_failed = 1;
    c_sleep_ms(3);

    struct _pulse pulse;
    memset(&pulse, 0, sizeof pulse);
    if (MsgReceivePulse(chid, &pulse, sizeof pulse, NULL) != 0)
        g_failed = 1;
    if (pulse.code != _PULSE_CODE_UNBLOCK)
        g_failed = 1;

    if (MsgReply(rv, 0, NULL, 0) != 0) g_failed = 1;
    pthread_join(client, NULL);
    ConnectDetach(g_coid);
    ChannelDestroy(chid);
}

/* Phase 2: no unblock contract; the client leaves and the late reply is ESRCH. */
static void phase_esrch(void)
{
    int chid = ChannelCreate(0);
    if (chid < 0) { g_failed = 1; return; }
    g_coid = ConnectAttach(0, 0, chid, 0, 0);
    if (g_coid < 0) { g_failed = 1; return; }

    pthread_t client;
    if (pthread_create(&client, NULL, esrch_client, NULL) != 0) {
        g_failed = 1;
        return;
    }

    rcvid_t rv = MsgReceive(chid, NULL, 0, NULL);
    if (rv <= 0) g_failed = 1;
    c_sleep_ms(3); // the client's deadline passes and the thread exits
    if (MsgReply(rv, 0, NULL, 0) != -1 || errno != ESRCH)
        g_failed = 1;
    if (!g_timed_out) g_failed = 1;

    pthread_join(client, NULL);
    ConnectDetach(g_coid);
    ChannelDestroy(chid);
}

int permute_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    phase_unblock();
    phase_esrch();
    return g_failed ? 1 : 0;
}

#ifdef __QNXNTO__
int main(int argc, char **argv) { return permute_main(argc, argv); }
#endif
