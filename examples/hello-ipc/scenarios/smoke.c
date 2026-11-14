// SPDX-License-Identifier: Apache-2.0
//
// examples/hello-ipc: the smallest useful QNX message-passing program.
//
// One server thread owns a channel and three clients attach to it. Each client
// sends a request and checks the reply, so a clean run is a real assertion
// about ordering and payload identity, not just "the process exited".
//
// What it demonstrates:
//   ChannelCreate, ConnectAttach, MsgSend, MsgReceive, MsgReply,
//   ConnectDetach, ChannelDestroy, and the thread ABI.
//
// Build and run (from this directory, after `make` at the repository root):
//   ../../tools/permute build
//   ../../tools/permute run --guest .permute/build/proc_smoke__0.so
//   ../../tools/permute show --seed 0

#include <pthread.h>
#include <string.h>

#include "abi/permute_iface.h"

#define REQUESTS 3

// The message is its own reply: the server adds one to `value` and echoes the
// whole struct, so the client can check both the payload and the message size.
typedef struct {
    int value;
    int parity;
} request_t;

static int g_chid = -1;
static volatile int g_failed;

static void *server_thread(void *arg)
{
    (void)arg;

    for (int i = 0; i < REQUESTS; i++) {
        request_t msg;
        struct _msg_info info;
        memset(&msg, 0, sizeof msg);

        rcvid_t rv = MsgReceive(g_chid, &msg, sizeof msg, &info);
        if (rv <= 0 || info.msglen != (int)sizeof msg) {
            g_failed = 1;
            break;
        }
        msg.value += 1;
        msg.parity = msg.value & 1;
        if (MsgReply(rv, 0, &msg, sizeof msg) != 0)
            g_failed = 1;
    }
    return NULL;
}

static void *client_thread(void *arg)
{
    int base = *(const int *)arg;

    int coid = ConnectAttach(0, 0, g_chid, 0, 0);
    if (coid < 0) {
        g_failed = 1;
        return NULL;
    }

    for (int i = 0; i < REQUESTS; i++) {
        request_t req = { base + i, -1 };
        request_t reply;
        memset(&reply, 0, sizeof reply);

        if (MsgSend(coid, &req, sizeof req, &reply, sizeof reply) != 0) {
            g_failed = 1;
            break;
        }
        if (reply.value != req.value + 1 || reply.parity != (reply.value & 1))
            g_failed = 1;
    }

    ConnectDetach(coid);
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
    pthread_t client;
    int base = 10;

    if (pthread_create(&server, NULL, server_thread, NULL) != 0)
        return 1;
    if (pthread_create(&client, NULL, client_thread, &base) != 0)
        return 1;

    pthread_join(client, NULL);
    pthread_join(server, NULL);

    ChannelDestroy(g_chid);
    return g_failed ? 1 : 0;
}

#ifdef __QNXNTO__
int main(int argc, char **argv) { return permute_main(argc, argv); }
#endif
