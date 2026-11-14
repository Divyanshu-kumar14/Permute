// SPDX-License-Identifier: Apache-2.0
// C15 (optional) — ChannelDestroy unblocks queued senders with ESRCH.
#include "conformance/scenarios/common.h"

#define SENDERS 2

static int g_chid = -1;
static volatile int g_failed;
static volatile int g_senders_esrch;

static void *sender(void *arg)
{
    (void)arg;
    int coid = ConnectAttach(0, 0, g_chid, 0, 0);
    if (coid < 0) { g_failed = 1; return NULL; }
    if (MsgSend(coid, "s", 1, NULL, 0) == -1 && errno == ESRCH)
        g_senders_esrch++;
    ConnectDetach(coid);
    return NULL;
}

int permute_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    g_chid = ChannelCreate(0);
    if (g_chid < 0) return 1;

    pthread_t senders[SENDERS];
    for (int i = 0; i < SENDERS; i++)
        if (pthread_create(&senders[i], NULL, sender, NULL) != 0) return 1;

    c_sleep_ms(1); // both are SEND-blocked on the queue
    if (ChannelDestroy(g_chid) != 0) g_failed = 1;

    for (int i = 0; i < SENDERS; i++) pthread_join(senders[i], NULL);
    if (g_senders_esrch != SENDERS) g_failed = 1;
    return g_failed ? 1 : 0;
}

#ifdef __QNXNTO__
int main(int argc, char **argv) { return permute_main(argc, argv); }
#endif
