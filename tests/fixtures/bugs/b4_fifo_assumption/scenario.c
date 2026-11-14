// SPDX-License-Identifier: Apache-2.0
//
// B4 — cross-priority FIFO assumption: the server assumes the first message it
// receives came from the first sender.
//
// Two clients send while the server is busy: L at priority 5, H at priority
// 15. The receive queue is priority-ordered, so H is served first. The buggy
// server checks the first message against L's payload and fails; the fix
// identifies each message by its own content.
//
// Three seeds in four include the high-priority sender.

#include <pthread.h>
#include <sched.h>
#include <string.h>

#include "abi/permute_iface.h"

static int g_chid = -1;
static volatile int g_failed;
static int g_race;

static void set_prio(int priority)
{
    struct sched_param param;
    memset(&param, 0, sizeof param);
    param.sched_priority = priority;
    if (SchedSet(0, 0, SCHED_FIFO, &param) != 0)
        g_failed = 1;
}

static void *client_thread(void *arg)
{
    const char *letter = arg;
    set_prio(letter[0] == 'H' ? 15 : 5);

    int coid = ConnectAttach(0, 0, g_chid, 0, 0);
    if (coid < 0) {
        g_failed = 1;
        return NULL;
    }

    unsigned char reply[8];
    if (MsgSend(coid, letter, 1, reply, sizeof reply) != 0)
        g_failed = 1;
    ConnectDetach(coid);
    return NULL;
}

static void *server_thread(void *arg)
{
    (void)arg;
    set_prio(10);

    // Let both clients queue before the first receive.
    struct timespec settle = { 0, 1000 * 1000 };
    (void)nanosleep(&settle, NULL);

    unsigned char msg[16];
    struct _msg_info info;
    rcvid_t rv = MsgReceive(g_chid, msg, sizeof msg, &info);
    if (rv <= 0) {
        g_failed = 1;
        return NULL;
    }

    if (msg[0] != 'L') {
        // Defect: the first arrival is assumed to be the low-priority client.
        g_failed = 1;
    }
    (void)MsgReply(rv, 0, msg, info.msglen);

    if (g_race) {
        struct sigevent unblock;
        memset(&unblock, 0, sizeof unblock);
        SIGEV_UNBLOCK_INIT(&unblock);
        uint64_t ntime = 1000ull * 1000ull;
        if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, &unblock,
                         &ntime, NULL) != 0)
            g_failed = 1;

        rv = MsgReceive(g_chid, msg, sizeof msg, &info);
        if (rv <= 0) {
            g_failed = 1;
            return NULL;
        }
        if (msg[0] != 'H') {
            g_failed = 1;
        }
        (void)MsgReply(rv, 0, msg, info.msglen);
    }

    return NULL;
}

int permute_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    g_race = (rand() % 4) != 0;

    g_chid = ChannelCreate(0);
    if (g_chid < 0)
        return 1;

    pthread_t server, low, high;
    if (pthread_create(&server, NULL, server_thread, NULL) != 0)
        return 1;
    if (pthread_create(&low, NULL, client_thread, (void *)"L") != 0)
        return 1;
    if (g_race && pthread_create(&high, NULL, client_thread, (void *)"H") != 0)
        return 1;

    (void)pthread_join(low, NULL);
    if (g_race)
        (void)pthread_join(high, NULL);
    (void)pthread_join(server, NULL);

    ChannelDestroy(g_chid);
    return g_failed ? 1 : 0;
}
