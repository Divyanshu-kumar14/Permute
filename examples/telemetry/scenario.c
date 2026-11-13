// SPDX-License-Identifier: Apache-2.0
//
// examples/telemetry: the sensor-fanout workload.
//
// One server, three clients at priorities 5/15/25, a collector that receives
// pulses, a sampling timer, a client timeout, and a client kill. This is the
// scenario that is meant to touch every modelled primitive at least once; the
// static check in tools/build_scenarios.py enforces that.

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <time.h>

#include "abi/permute_iface.h"

#define CLIENTS 3
#define MESSAGES 20
#define SAMPLE_NS (5ull * 1000ull * 1000ull)

static int g_srv_chid = -1;
static int g_col_chid = -1;
static int g_srv_coid = -1;
static int g_col_coid = -1;
static volatile int g_failed;
static volatile int g_stop;

static void set_prio(int priority)
{
    struct sched_param param;
    memset(&param, 0, sizeof param);
    param.sched_priority = priority;
    if (SchedSet(0, 0, SCHED_FIFO, &param) != 0)
        g_failed = 1;
}

static void *collector_thread(void *arg)
{
    (void)arg;

    for (;;) {
        struct _pulse pulse;
        memset(&pulse, 0, sizeof pulse);
        if (MsgReceivePulse(g_col_chid, &pulse, sizeof pulse, NULL) != 0)
            break;
        if (pulse.code == _PULSE_CODE_DISCONNECT)
            break;
    }
    return NULL;
}

static void *server_thread(void *arg)
{
    (void)arg;
    set_prio(15);

    // Clients may register this event for asynchronous notification.
    struct sigevent event;
    SIGEV_PULSE_INIT(&event, g_col_coid, 8, 0x40, 0);
    if (MsgRegisterEvent(&event, g_col_coid) != 0)
        g_failed = 1;

    struct sigevent sample;
    SIGEV_PULSE_INIT(&sample, g_col_coid, 9, 0x41, 0);

    uint64_t ntime = SAMPLE_NS;
    uint64_t otime = 0;
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, &sample, &ntime,
                     &otime) != 0)
        g_failed = 1;

    for (int i = 0; i < CLIENTS * MESSAGES && !g_stop; i++) {
        char msg[64];
        struct _msg_info info;

        rcvid_t rv = MsgReceive(g_srv_chid, msg, sizeof msg, &info);
        if (rv == 0) {
            // A pulse: the sampling timer or a client notification.
            (void)MsgVerifyEvent(0, &event);
            (void)TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, &sample,
                               &ntime, &otime);
            continue;
        }
        if (rv < 0) {
            if (errno == ETIMEDOUT) {
                (void)MsgSendPulse(g_col_coid, -1, 0x42, 0);
                (void)TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE,
                                   &sample, &ntime, &otime);
                continue;
            }
            break;
        }

        if (info.msglen > 0 && msg[0] == 'E') {
            (void)MsgError(rv, EINVAL);
        } else {
            (void)MsgDeliverEvent(rv, &event);
            (void)MsgReply(rv, 0, msg, info.msglen);
        }
        (void)SchedYield();
    }

    g_stop = 1;
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

    for (int i = 0; i < MESSAGES && !g_stop; i++) {
        char msg[64];
        char reply[64];
        memset(msg, priority, sizeof msg);
        msg[0] = (priority == 25 && i == 0) ? 'E' : 'D';

        if (MsgSend(coid, msg, sizeof msg, reply, sizeof reply) != 0 &&
            msg[0] != 'E')
            g_failed = 1;
    }

    struct sched_param observed;
    memset(&observed, 0, sizeof observed);
    (void)SchedGet(0, 0, &observed);

    ConnectDetach(coid);
    return NULL;
}

int permute_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    g_srv_chid = ChannelCreate(_NTO_CHF_DISCONNECT | _NTO_CHF_UNBLOCK);
    g_col_chid = ChannelCreate(0);
    if (g_srv_chid < 0 || g_col_chid < 0)
        return 1;

    g_srv_coid = ConnectAttach(0, 0, g_srv_chid, 0, 0);
    g_col_coid = ConnectAttach(0, 0, g_col_chid, 0, 0);
    if (g_srv_coid < 0 || g_col_coid < 0)
        return 1;

    static const int priorities[CLIENTS] = { 5, 15, 25 };

    pthread_t collector, server, clients[CLIENTS];
    if (pthread_create(&collector, NULL, collector_thread, NULL) != 0)
        return 1;
    if (pthread_create(&server, NULL, server_thread, NULL) != 0)
        return 1;
    for (int i = 0; i < CLIENTS; i++) {
        if (pthread_create(&clients[i], NULL, client_thread,
                           (void *)&priorities[i]) != 0)
            return 1;
    }

    // Let the fanout run briefly, then kill one client mid-send.
    struct timespec nap = { 0, 20 * 1000 * 1000 };
    (void)nanosleep(&nap, NULL);
    (void)pthread_cancel(clients[CLIENTS - 1]);

    for (int i = 0; i < CLIENTS; i++)
        pthread_join(clients[i], NULL);

    g_stop = 1;
    (void)MsgSendPulse(g_srv_coid, -1, _PULSE_CODE_DISCONNECT, 0);
    (void)MsgSendPulse(g_col_coid, -1, _PULSE_CODE_DISCONNECT, 0);

    pthread_join(server, NULL);
    pthread_join(collector, NULL);

    ConnectDetach(g_col_coid);
    ConnectDetach(g_srv_coid);
    ChannelDestroy(g_col_chid);
    ChannelDestroy(g_srv_chid);
    return g_failed ? 1 : 0;
}
