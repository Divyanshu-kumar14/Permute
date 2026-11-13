// SPDX-License-Identifier: Apache-2.0
//
// s06_unblock_pulse: the _NTO_CHF_UNBLOCK contract.
//
// A client in REPLY whose timeout expires does not unblock when the server
// asked for notification. The server receives _PULSE_CODE_UNBLOCK carrying the
// rcvid, and its reply completes the original send normally.

#include <errno.h>
#include <string.h>

#include "abi/permute_iface.h"

static int g_chid = -1;
static int g_coid = -1;
static volatile int g_failed;

static void *server_thread(void *arg)
{
    (void)arg;

    rcvid_t rv = MsgReceive(g_chid, NULL, 0, NULL);
    if (rv <= 0) {
        g_failed = 1;
        return NULL;
    }

    // Give the client's timeout time to expire while it is REPLY-blocked.
    struct timespec nap = { 0, 30 * 1000 * 1000 };
    (void)nanosleep(&nap, NULL);

    struct _pulse pulse;
    memset(&pulse, 0, sizeof pulse);
    if (MsgReceivePulse(g_chid, &pulse, sizeof pulse, NULL) != 0)
        g_failed = 1;
    if (pulse.code != _PULSE_CODE_UNBLOCK)
        g_failed = 1;
    if ((rcvid_t)pulse.value.sival_int != rv)
        g_failed = 1;

    (void)MsgReply(rv, 0, NULL, 0);
    return NULL;
}

static void *client_thread(void *arg)
{
    (void)arg;

    struct sigevent unblock;
    memset(&unblock, 0, sizeof unblock);
    SIGEV_UNBLOCK_INIT(&unblock);

    uint64_t ntime = 10ull * 1000ull * 1000ull;
    if (TimerTimeout(CLOCK_MONOTONIC,
                     _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, &unblock, &ntime,
                     NULL) != 0)
        g_failed = 1;

    // The timeout fires while the server holds the message, so the send ends
    // when the server replies, not when the timer expires.
    if (MsgSend(g_coid, "x", 1, NULL, 0) != 0)
        g_failed = 1;
    return NULL;
}

int permute_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    g_chid = ChannelCreate(_NTO_CHF_UNBLOCK);
    if (g_chid < 0)
        return 1;
    g_coid = ConnectAttach(0, 0, g_chid, 0, 0);
    if (g_coid < 0)
        return 1;

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
