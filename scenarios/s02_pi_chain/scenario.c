// SPDX-License-Identifier: Apache-2.0
//
// s02_pi_chain: a three-hop inheritance chain.
//
// client (10) -> server (5) -> driver (5). The server must run at 10 while it
// serves, and the message it forwards must carry that priority so the driver
// reaches 10 as well. No donor list exists anywhere; the propagation is a
// consequence of sending at the inherited priority.

#include <pthread.h>
#include <sched.h>
#include <string.h>

#include "abi/permute_iface.h"

static int g_server_chid = -1;
static int g_driver_chid = -1;
static int g_to_driver = -1;
static volatile int g_driver_prio;
static volatile int g_failed;

static void set_prio(int priority)
{
    struct sched_param param;
    memset(&param, 0, sizeof param);
    param.sched_priority = priority;
    if (SchedSet(0, 0, SCHED_FIFO, &param) != 0)
        g_failed = 1;
}

static void *driver_thread(void *arg)
{
    (void)arg;
    set_prio(5);

    unsigned char msg[32];
    struct _msg_info info;
    rcvid_t rv = MsgReceive(g_driver_chid, msg, sizeof msg, &info);
    if (rv <= 0) {
        g_failed = 1;
        return NULL;
    }

    struct sched_param observed;
    memset(&observed, 0, sizeof observed);
    if (SchedGet(0, 0, &observed) == 0)
        g_driver_prio = observed.sched_priority;

    (void)MsgReply(rv, 0, msg, info.msglen);
    return NULL;
}

static void *server_thread(void *arg)
{
    (void)arg;
    set_prio(5);

    unsigned char msg[32];
    unsigned char reply[32];
    struct _msg_info info;
    rcvid_t rv = MsgReceive(g_server_chid, msg, sizeof msg, &info);
    if (rv <= 0) {
        g_failed = 1;
        return NULL;
    }

    // Forward at the inherited priority; that is what makes the chain work.
    long forwarded = MsgSend(g_to_driver, msg, info.msglen, reply,
                             sizeof reply);
    if (forwarded != 0) {
        g_failed = 1;
        (void)MsgError(rv, 5);
        return NULL;
    }

    (void)MsgReply(rv, 0, reply, sizeof reply);
    return NULL;
}

static void *client_thread(void *arg)
{
    (void)arg;
    set_prio(10);

    int client_chid = ChannelCreate(0);
    if (client_chid < 0) {
        g_failed = 1;
        return NULL;
    }
    int coid = ConnectAttach(0, 0, g_server_chid, 0, 0);
    if (coid < 0) {
        g_failed = 1;
        return NULL;
    }

    unsigned char msg[32];
    unsigned char reply[32];
    memset(msg, 0x5A, sizeof msg);
    if (MsgSend(coid, msg, sizeof msg, reply, sizeof reply) != 0)
        g_failed = 1;

    ConnectDetach(coid);
    ChannelDestroy(client_chid);
    return NULL;
}

int permute_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    g_server_chid = ChannelCreate(0);
    g_driver_chid = ChannelCreate(0);
    if (g_server_chid < 0 || g_driver_chid < 0)
        return 1;

    g_to_driver = ConnectAttach(0, 0, g_driver_chid, 0, 0);
    if (g_to_driver < 0)
        return 1;

    pthread_t driver, server, client;
    if (pthread_create(&driver, NULL, driver_thread, NULL) != 0)
        return 1;
    if (pthread_create(&server, NULL, server_thread, NULL) != 0)
        return 1;
    if (pthread_create(&client, NULL, client_thread, NULL) != 0)
        return 1;

    pthread_join(client, NULL);
    pthread_join(server, NULL);
    pthread_join(driver, NULL);

    // The driver must have observed the client's priority, not its own.
    if (g_driver_prio != 10)
        g_failed = 1;

    ConnectDetach(g_to_driver);
    ChannelDestroy(g_server_chid);
    ChannelDestroy(g_driver_chid);
    return g_failed ? 1 : 0;
}
