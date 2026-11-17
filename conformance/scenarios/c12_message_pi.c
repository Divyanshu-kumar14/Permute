// SPDX-License-Identifier: Apache-2.0
// C12 — message-driven priority inheritance, and the FIXED_PRIORITY variant
// that disables it.
#include "conformance/scenarios/common.h"

static volatile int g_failed;
static int g_chid = -1;
static int g_coid = -1;
static volatile int g_seen;

static void *server_thread(void *arg)
{
    (void)arg;
    c_set_prio(5);
    rcvid_t rv = MsgReceive(g_chid, NULL, 0, NULL);
    if (rv <= 0) { g_failed = 1; return NULL; }
    struct sched_param param;
    memset(&param, 0, sizeof param);
    // QNX returns the scheduling policy on success and -1 on error; the
    // POSIX sched_getparam() convention of 0 does not apply.
    if (SchedGet(0, 0, &param) != -1)
        g_seen = param.sched_priority;
    if (MsgReply(rv, 0, NULL, 0) != 0) g_failed = 1;
    return NULL;
}

static int run_phase(int flags, int expect)
{
    g_seen = 0;
    g_chid = ChannelCreate(flags);
    if (g_chid < 0) return 1;
    g_coid = ConnectAttach(0, 0, g_chid, 0, 0);
    if (g_coid < 0) return 1;

    pthread_t server;
    if (pthread_create(&server, NULL, server_thread, NULL) != 0) return 1;

    c_set_prio(20);
    if (MsgSend(g_coid, "p", 1, NULL, 0) != 0) g_failed = 1;
    pthread_join(server, NULL);

    if (g_seen != expect) g_failed = 1;

    ConnectDetach(g_coid);
    ChannelDestroy(g_chid);
    return 0;
}

int permute_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    // Inheriting channel: the prio-20 client's priority reaches the server.
    if (run_phase(0, 20) != 0) g_failed = 1;
    // Fixed-priority channel: the server keeps its own 5.
    if (run_phase(_NTO_CHF_FIXED_PRIORITY, 5) != 0) g_failed = 1;
    return g_failed ? 1 : 0;
}

#ifdef __QNXNTO__
int main(int argc, char **argv) { return permute_main(argc, argv); }
#endif
