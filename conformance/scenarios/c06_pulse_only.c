// SPDX-License-Identifier: Apache-2.0
// C06 (optional) — a pulse-only waiter must not consume a queued message.
#include "conformance/scenarios/common.h"

static int g_chid = -1;
static volatile int g_failed;
static volatile int g_message_taken;

static void *pulse_waiter(void *arg)
{
    (void)arg;
    c_set_prio(20);
    struct _pulse pulse;
    memset(&pulse, 0, sizeof pulse);
    if (MsgReceivePulse(g_chid, &pulse, sizeof pulse, NULL) != 0)
        g_failed = 1;
    if (pulse.code != 0x46) g_failed = 1;
    return NULL;
}

static void *sender(void *arg)
{
    (void)arg;
    c_set_prio(10);
    int coid = ConnectAttach(0, 0, g_chid, 0, 0);
    if (coid < 0) { g_failed = 1; return NULL; }
    if (MsgSend(coid, "q", 1, NULL, 0) != 0) g_failed = 1;
    ConnectDetach(coid);
    return NULL;
}

static void *general_receiver(void *arg)
{
    (void)arg;
    c_set_prio(20);
    char msg[8];
    rcvid_t rv = MsgReceive(g_chid, msg, sizeof msg, NULL);
    if (rv <= 0 || msg[0] != 'q') { g_failed = 1; return NULL; }
    g_message_taken = 1;
    if (MsgReply(rv, 0, NULL, 0) != 0) g_failed = 1;
    return NULL;
}

int permute_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    g_chid = ChannelCreate(0);
    if (g_chid < 0) return 1;
    int coid = ConnectAttach(0, 0, g_chid, 0, 0);
    if (coid < 0) return 1;

    pthread_t pulse, send;
    if (pthread_create(&pulse, NULL, pulse_waiter, NULL) != 0) return 1;
    if (pthread_create(&send, NULL, sender, NULL) != 0) return 1;

    c_sleep_ms(1); // the message is queued; the pulse-only waiter ignores it
    if (MsgSendPulse(coid, 30, 0x46, 0) != 0) g_failed = 1;
    pthread_join(pulse, NULL);

    pthread_t receiver;
    if (pthread_create(&receiver, NULL, general_receiver, NULL) != 0) return 1;
    pthread_join(receiver, NULL);
    pthread_join(send, NULL);

    if (!g_message_taken) g_failed = 1;
    ConnectDetach(coid);
    ChannelDestroy(g_chid);
    return g_failed ? 1 : 0;
}

#ifdef __QNXNTO__
int main(int argc, char **argv) { return permute_main(argc, argv); }
#endif
