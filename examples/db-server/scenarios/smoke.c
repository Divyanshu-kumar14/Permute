// SPDX-License-Identifier: Apache-2.0
//
// examples/db-server: a key/value server under three concurrent clients.
//
// What it demonstrates beyond hello-ipc:
//   * a multi-threaded server that serves three clients at priorities 5/10/15;
//   * message-driven priority inheritance, observed from inside the guest with
//     SchedGet;
//   * a request whose key is absent, answered with MsgError(ENOENT), which the
//     client sees as -1/errno rather than as a successful reply;
//   * seeded request selection with rand(), so different seeds produce
//     different request orders and different traces (LIMITS #25);
//   * completion pulses and a TimerTimeout receive deadline on the main thread;
//   * a clean shutdown: the main thread pulses _PULSE_CODE_DISCONNECT and the
//     server exits its receive loop on the pulse.
//
// Build and run (from this directory, after `make` at the repository root):
//   ../../tools/permute build
//   ../../tools/permute run --guest .permute/build/proc_smoke__0.so
//   ../../tools/permute show --seed 0

#include <errno.h>
#include <pthread.h>
#include <string.h>

#include "abi/permute_iface.h"

#define CLIENTS 3
#define REQUESTS 6
#define SLOTS 8

enum {
    OP_GET = 0,
    OP_PUT = 1,
    OP_DELETE = 2,
};

typedef struct {
    int op;
    int key;
    int value;
    int client_prio;
} request_t;

typedef struct {
    int status;
    int value;
    int server_prio;
} reply_t;

static int g_srv_chid = -1;
static int g_done_chid = -1;
static int g_srv_coid = -1;
static int g_done_coid = -1;
static volatile int g_failed;
static volatile int g_stop;

static int g_slot_key[SLOTS];
static int g_slot_value[SLOTS];
static int g_slot_used[SLOTS];

static void set_prio(int priority)
{
    struct sched_param param;
    memset(&param, 0, sizeof param);
    param.sched_priority = priority;
    if (SchedSet(0, 0, SCHED_FIFO, &param) != 0)
        g_failed = 1;
}

static int current_prio(void)
{
    struct sched_param param;
    memset(&param, 0, sizeof param);
    if (SchedGet(0, 0, &param) != 0)
        return -1;
    return param.sched_priority;
}

static int slot_for(int key)
{
    for (int i = 0; i < SLOTS; i++)
        if (g_slot_used[i] && g_slot_key[i] == key)
            return i;
    for (int i = 0; i < SLOTS; i++)
        if (!g_slot_used[i])
            return i;
    return -1;
}

static void *server_thread(void *arg)
{
    (void)arg;
    set_prio(10);

    while (!g_stop) {
        request_t req;
        reply_t reply;
        struct _msg_info info;
        memset(&req, 0, sizeof req);
        memset(&reply, 0, sizeof reply);

        rcvid_t rv = MsgReceive(g_srv_chid, &req, sizeof req, &info);
        if (rv < 0) {
            if (errno == ETIMEDOUT)
                continue;
            g_failed = 1;
            break;
        }
        if (rv == 0) {
            // Pulse: the disconnect notice is the shutdown request.
            g_stop = 1;
            break;
        }
        if (info.msglen != (int)sizeof req) {
            (void)MsgError(rv, EMSGSIZE);
            continue;
        }

        int server_prio = current_prio();
        if (server_prio < req.client_prio)
            g_failed = 1;

        int slot = slot_for(req.key);
        switch (req.op) {
        case OP_GET:
            if (slot < 0 || !g_slot_used[slot]) {
                (void)MsgError(rv, ENOENT);
                continue;
            }
            reply.value = g_slot_value[slot];
            break;
        case OP_PUT:
            if (slot < 0) {
                (void)MsgError(rv, ENOSPC);
                continue;
            }
            g_slot_used[slot] = 1;
            g_slot_key[slot] = req.key;
            g_slot_value[slot] = req.value;
            reply.value = req.value;
            break;
        case OP_DELETE:
            if (slot < 0 || !g_slot_used[slot]) {
                (void)MsgError(rv, ENOENT);
                continue;
            }
            g_slot_used[slot] = 0;
            reply.value = 0;
            break;
        default:
            (void)MsgError(rv, EINVAL);
            continue;
        }

        reply.server_prio = server_prio;
        if (MsgReply(rv, 0, &reply, sizeof reply) != 0)
            g_failed = 1;
        (void)SchedYield();
    }
    return NULL;
}

static void *client_thread(void *arg)
{
    int priority = *(const int *)arg;
    set_prio(priority);

    int coid = ConnectAttach(0, 0, g_srv_chid, 0, 0);
    if (coid < 0) {
        g_failed = 1;
        return NULL;
    }

    // rand() is seeded by the kernel's per-run table, so the request order is a
    // function of the seed and the trace differs across seeds.
    int wrote = 0;
    for (int i = 0; i < REQUESTS; i++) {
        request_t req;
        reply_t reply;
        memset(&req, 0, sizeof req);
        memset(&reply, 0, sizeof reply);

        req.key = (priority * 7 + rand() % 6) % 12;
        req.op = (int)(rand() % 3);
        req.value = 100 + rand() % 900;
        req.client_prio = priority;

        int rc = MsgSend(coid, &req, sizeof req, &reply, sizeof reply);
        if (req.op == OP_GET && rc != 0 && errno != ENOENT && errno != ENOSPC) {
            // A GET may legitimately miss; anything else is a protocol error.
            g_failed = 1;
        } else if (rc == 0) {
            if (reply.server_prio < priority)
                g_failed = 1;
            if (req.op == OP_PUT) {
                wrote++;
                if (reply.value != req.value)
                    g_failed = 1;
            }
        }
    }

    // A delete of a key that was never written must be rejected with ENOENT.
    request_t missing;
    reply_t ignored;
    memset(&missing, 0, sizeof missing);
    memset(&ignored, 0, sizeof ignored);
    missing.op = OP_DELETE;
    missing.key = 1000 + priority;
    missing.client_prio = priority;
    if (MsgSend(coid, &missing, sizeof missing, &ignored, sizeof ignored) != -1
        || errno != ENOENT)
        g_failed = 1;

    // A short message must be rejected with EMSGSIZE, not read as a request
    // with uninitialised fields.
    int shorty = priority;
    if (MsgSend(coid, &shorty, sizeof shorty, &ignored, sizeof ignored) != -1
        || errno != EMSGSIZE)
        g_failed = 1;

    if (MsgSendPulse(g_done_coid, -1, 0x51, priority) != 0)
        g_failed = 1;
    ConnectDetach(coid);
    (void)wrote;
    return NULL;
}

int permute_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    memset(g_slot_used, 0, sizeof g_slot_used);

    g_srv_chid = ChannelCreate(_NTO_CHF_DISCONNECT | _NTO_CHF_UNBLOCK);
    g_done_chid = ChannelCreate(0);
    if (g_srv_chid < 0 || g_done_chid < 0)
        return 1;

    g_srv_coid = ConnectAttach(0, 0, g_srv_chid, 0, 0);
    g_done_coid = ConnectAttach(0, 0, g_done_chid, 0, 0);
    if (g_srv_coid < 0 || g_done_coid < 0)
        return 1;

    pthread_t server, clients[CLIENTS];
    static const int priorities[CLIENTS] = { 5, 10, 15 };

    if (pthread_create(&server, NULL, server_thread, NULL) != 0)
        return 1;
    for (int i = 0; i < CLIENTS; i++) {
        if (pthread_create(&clients[i], NULL, client_thread,
                           (void *)&priorities[i]) != 0)
            return 1;
    }

    for (int i = 0; i < CLIENTS; i++)
        pthread_join(clients[i], NULL);

    // Wait for the three completion pulses under a deadline, then stop the
    // server with a disconnect pulse.
    struct sigevent unblock;
    memset(&unblock, 0, sizeof unblock);
    SIGEV_UNBLOCK_INIT(&unblock);
    uint64_t ntime = 100ull * 1000ull * 1000ull;
    uint64_t otime = 0;
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, &unblock, &ntime,
                     &otime) != 0)
        g_failed = 1;

    int pulses = 0;
    while (pulses < CLIENTS) {
        struct _pulse pulse;
        memset(&pulse, 0, sizeof pulse);
        if (MsgReceivePulse(g_done_chid, &pulse, sizeof pulse, NULL) != 0) {
            g_failed = 1;
            break;
        }
        if (pulse.code == 0x51)
            pulses++;
    }

    g_stop = 1;
    (void)MsgSendPulse(g_srv_coid, -1, _PULSE_CODE_DISCONNECT, 0);
    pthread_join(server, NULL);

    ConnectDetach(g_done_coid);
    ConnectDetach(g_srv_coid);
    ChannelDestroy(g_done_chid);
    ChannelDestroy(g_srv_chid);
    return g_failed ? 1 : 0;
}

#ifdef __QNXNTO__
int main(int argc, char **argv) { return permute_main(argc, argv); }
#endif
