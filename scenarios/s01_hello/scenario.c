// SPDX-License-Identifier: Apache-2.0
//
// s01_hello: the smallest real exchange.
//
// One server channel, one client connection, 10,000 round trips of a 64-byte
// payload. This is the scenario every other one is built on: if it fails, the
// message path itself is wrong.

#include <pthread.h>
#include <string.h>

#include "abi/permute_iface.h"

#define MESSAGES 10000
#define PAYLOAD 64

static int g_chid = -1;
static int g_coid = -1;
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

static void *client_thread(void *arg)
{
    (void)arg;

    for (int i = 0; i < MESSAGES; i++) {
        unsigned char out[PAYLOAD];
        unsigned char in[PAYLOAD];

        memset(out, i & 0xFF, sizeof out);
        if (MsgSend(g_coid, out, sizeof out, in, sizeof in) != 0) {
            g_failed = 1;
            return NULL;
        }
        if (memcmp(out, in, sizeof out) != 0) {
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

    g_coid = ConnectAttach(0, 0, g_chid, 0, 0);
    if (g_coid < 0) {
        ChannelDestroy(g_chid);
        return 1;
    }

    pthread_t server, client;
    if (pthread_create(&server, NULL, server_thread, NULL) != 0)
        return 1;
    if (pthread_create(&client, NULL, client_thread, NULL) != 0)
        return 1;

    pthread_join(client, NULL);
    pthread_join(server, NULL);

    ConnectDetach(g_coid);
    ChannelDestroy(g_chid);
    return g_failed ? 1 : 0;
}
