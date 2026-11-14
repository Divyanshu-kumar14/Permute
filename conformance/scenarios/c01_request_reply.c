// SPDX-License-Identifier: Apache-2.0
// C01 — request/reply, 1,000 messages. Exact.
#include "conformance/scenarios/common.h"

#define MESSAGES 1000

static int g_chid = -1;
static volatile int g_failed;

typedef struct { int index; } payload_t;

static void *server_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < MESSAGES; i++) {
        payload_t msg;
        struct _msg_info info;
        rcvid_t rv = MsgReceive(g_chid, &msg, sizeof msg, &info);
        if (rv <= 0 || info.msglen != (int)sizeof msg || msg.index != i)
            g_failed = 1;
        if (MsgReply(rv, 0, &msg, sizeof msg) != 0)
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

    int coid = ConnectAttach(0, 0, g_chid, 0, 0);
    if (coid < 0) return 1;

    for (int i = 0; i < MESSAGES; i++) {
        payload_t msg = { i }, reply = { -1 };
        if (MsgSend(coid, &msg, sizeof msg, &reply, sizeof reply) != 0 ||
            reply.index != i)
            g_failed = 1;
    }

    pthread_join(server, NULL);
    ConnectDetach(coid);
    ChannelDestroy(g_chid);
    return g_failed ? 1 : 0;
}

#ifdef __QNXNTO__
int main(int argc, char **argv) { return permute_main(argc, argv); }
#endif
