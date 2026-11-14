// SPDX-License-Identifier: Apache-2.0
//
// G1/NFR-2 probe: request/reply round trips with tracing disabled.
#include <pthread.h>
#include <sched.h>
#include <string.h>

#include "abi/permute_iface.h"

#define MESSAGES 100000
#define PAYLOAD 64

static int g_chid = -1;
static volatile int g_failed;

static void *server_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < MESSAGES; i++) {
        unsigned char msg[PAYLOAD];
        struct _msg_info info;
        rcvid_t rv = MsgReceive(g_chid, msg, sizeof msg, &info);
        if (rv <= 0) {
            g_failed = 1;
            return NULL;
        }
        if (MsgReply(rv, 0, msg, info.msglen) != 0) {
            g_failed = 1;
            return NULL;
        }
    }
    return NULL;
}

int permute_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    g_chid = ChannelCreate(0);
    if (g_chid < 0)
        return 1;

    pthread_t server;
    if (pthread_create(&server, NULL, server_thread, NULL) != 0)
        return 1;

    int coid = ConnectAttach(0, 0, g_chid, 0, 0);
    if (coid < 0)
        return 1;

    unsigned char msg[PAYLOAD];
    unsigned char reply[PAYLOAD];
    memset(msg, 0x5A, sizeof msg);
    for (int i = 0; i < MESSAGES; i++) {
        if (MsgSend(coid, msg, sizeof msg, reply, sizeof reply) != 0 ||
            memcmp(msg, reply, sizeof msg) != 0) {
            g_failed = 1;
            break;
        }
    }

    pthread_join(server, NULL);
    ConnectDetach(coid);
    ChannelDestroy(g_chid);
    return g_failed ? 1 : 0;
}
