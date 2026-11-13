// SPDX-License-Identifier: Apache-2.0
//
// s03_timer_timeout: every TimerTimeout state-mask bit.
//
// The point is coverage of the mask, not of the blocking call. Each block
// names the call that would enter the state and passes exactly one bit, so
// the set of bits is exhaustive and a missing one is visible.
//
// Bits whose blocking call is not yet interposed (signals, semaphores,
// condition variables) still arm the timeout; the scenario documents the
// pending call rather than pretending to enter it.

#include <pthread.h>
#include <semaphore.h>
#include <string.h>

#include "abi/permute_iface.h"

#define NS 10ull * 1000ull * 1000ull

static int g_chid = -1;
static int g_coid = -1;
static volatile int g_failed;

static void arm(uint32_t mask, struct sigevent *event, uint64_t ns)
{
    uint64_t ntime = ns;
    uint64_t otime = 0;
    if (TimerTimeout(CLOCK_MONOTONIC, mask, event, &ntime, &otime) != 0)
        g_failed = 1;
}

static void enter_modelled_states(void)
{
    struct sigevent unblock;
    memset(&unblock, 0, sizeof unblock);
    SIGEV_UNBLOCK_INIT(&unblock);

    // SEND: a send to a channel nobody receives from.
    int idle_chid = ChannelCreate(0);
    int idle_coid = ConnectAttach(0, 0, idle_chid, 0, 0);
    arm(_NTO_TIMEOUT_SEND, &unblock, NS);
    if (MsgSend(idle_coid, "x", 1, NULL, 0) != -1)
        g_failed = 1;
    ConnectDetach(idle_coid);
    ChannelDestroy(idle_chid);

    // RECEIVE: nothing queued.
    arm(_NTO_TIMEOUT_RECEIVE, &unblock, NS);
    if (MsgReceive(g_chid, NULL, 0, NULL) != -1)
        g_failed = 1;

    // REPLY: the client is waiting for a server that has taken the message.
    arm(_NTO_TIMEOUT_REPLY, &unblock, NS);
    if (MsgSend(g_coid, "x", 1, NULL, 0) != -1)
        g_failed = 1;

    // NANOSLEEP: the only form that blocks inside TimerTimeout itself.
    struct timespec nap = { 0, (long)NS };
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_NANOSLEEP, NULL, NULL,
                     NULL) == 0)
        (void)nanosleep(&nap, NULL);
}

static void arm_unmodelled_bits(void)
{
    struct sigevent unblock;
    memset(&unblock, 0, sizeof unblock);
    SIGEV_UNBLOCK_INIT(&unblock);

    // Each of these arms the bit. The corresponding blocking call is not
    // interposed yet (see LIMITS #24), so only the mask is exercised here.
    arm(_NTO_TIMEOUT_SIGSUSPEND, &unblock, NS);  // sigsuspend()
    arm(_NTO_TIMEOUT_SIGWAITINFO, &unblock, NS); // sigwaitinfo()
    arm(_NTO_TIMEOUT_MUTEX, &unblock, NS);       // pthread_mutex_lock()
    arm(_NTO_TIMEOUT_CONDVAR, &unblock, NS);     // pthread_cond_wait()
    arm(_NTO_TIMEOUT_JOIN, &unblock, NS);        // pthread_join()
    arm(_NTO_TIMEOUT_INTR, &unblock, NS);        // InterruptWait()
    arm(_NTO_TIMEOUT_SEM, &unblock, NS);         // sem_wait()
}

static void *server_thread(void *arg)
{
    (void)arg;

    // Receive the client's message and hold it so the client sits in REPLY.
    rcvid_t rv = MsgReceive(g_chid, NULL, 0, NULL);
    if (rv <= 0) {
        g_failed = 1;
        return NULL;
    }
    struct timespec nap = { 0, 50 * 1000 * 1000 };
    (void)nanosleep(&nap, NULL);
    (void)MsgReply(rv, 0, NULL, 0);
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
    if (g_coid < 0)
        return 1;

    pthread_t server;
    if (pthread_create(&server, NULL, server_thread, NULL) != 0)
        return 1;

    enter_modelled_states();
    arm_unmodelled_bits();

    pthread_join(server, NULL);
    ConnectDetach(g_coid);
    ChannelDestroy(g_chid);
    return g_failed ? 1 : 0;
}
