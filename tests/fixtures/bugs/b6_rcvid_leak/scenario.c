// SPDX-License-Identifier: Apache-2.0
//
// B6 — client death: the server never releases the rcvid of a client that
// timed out and exited.
//
// Each short-lived client sends, misses its deadline, detaches, and exits. The
// server keeps the rcvid in a fixed table and never expires it. After more
// clients than slots the table is full and the final request cannot be served.
// The fix expires entries older than the client deadline.
//
// Three seeds in eight create more clients than slots.

#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <time.h>

#include "abi/permute_iface.h"

#define MAX_SLOTS 8

static int g_chid = -1;
static volatile int g_failed;
static volatile int g_clients;

static void set_prio(int priority)
{
    struct sched_param param;
    memset(&param, 0, sizeof param);
    param.sched_priority = priority;
    if (SchedSet(0, 0, SCHED_FIFO, &param) != 0)
        g_failed = 1;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void *server_thread(void *arg)
{
    (void)arg;
    set_prio(10);

    rcvid_t slots[MAX_SLOTS];
    uint64_t since[MAX_SLOTS];
    int used = 0;
    memset(slots, 0, sizeof slots);
    memset(since, 0, sizeof since);

    for (;;) {
        unsigned char msg[16];
        struct _msg_info info;
        rcvid_t rv = MsgReceive(g_chid, msg, sizeof msg, &info);
        if (rv <= 0) {
            g_failed = 1;
            return NULL;
        }

        if (msg[0] == 'C') {
            if (used < MAX_SLOTS) {
                slots[used] = rv;
                since[used] = now_ns();
                used++;
            }
            // Defect: entries of clients that timed out are never released.
            continue;
        }

        // Final request: a slot must exist for it.
        if (used >= MAX_SLOTS) {
            g_failed = 1; // table full: the leak lost a live client's slot
            (void)MsgError(rv, ENOMEM);
            return NULL;
        }
        (void)MsgReply(rv, 0, msg, info.msglen);
        return NULL;
    }
}

static void client_once(void)
{
    int coid = ConnectAttach(0, 0, g_chid, 0, 0);
    if (coid < 0) {
        g_failed = 1;
        return;
    }

    struct sigevent unblock;
    memset(&unblock, 0, sizeof unblock);
    SIGEV_UNBLOCK_INIT(&unblock);
    uint64_t ntime = 1000ull * 1000ull;
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_REPLY, &unblock, &ntime,
                     NULL) != 0)
        g_failed = 1;

    // The deadline is expected to pass; the server holds the rcvid forever.
    (void)MsgSend(coid, "C", 1, NULL, 0);
    ConnectDetach(coid);
}

int permute_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    g_clients = 3 + (rand() % 8);

    g_chid = ChannelCreate(0);
    if (g_chid < 0)
        return 1;

    pthread_t server;
    if (pthread_create(&server, NULL, server_thread, NULL) != 0)
        return 1;

    // Let the server reach MsgReceive before the first client sends.
    struct timespec settle = { 0, 1000 * 1000 };
    (void)nanosleep(&settle, NULL);

    for (int i = 0; i < g_clients; i++)
        client_once();

    int coid = ConnectAttach(0, 0, g_chid, 0, 0);
    if (coid < 0)
        return 1;
    unsigned char reply[8];
    if (MsgSend(coid, "F", 1, reply, sizeof reply) != 0)
        g_failed = 1;
    ConnectDetach(coid);

    (void)pthread_join(server, NULL);
    ChannelDestroy(g_chid);
    return g_failed ? 1 : 0;
}
