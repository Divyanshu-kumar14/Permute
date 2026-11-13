// SPDX-License-Identifier: Apache-2.0
//
// s04_pulse_drop: the short-buffer rule.
//
// MsgReceivePulse with a buffer smaller than struct _pulse does not truncate.
// It returns EFAULT and the pulse is lost. The second receive must therefore
// find nothing; a timeout keeps the scenario terminating.

#include <errno.h>
#include <string.h>

#include "abi/permute_iface.h"

static volatile int g_failed;

int permute_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    int chid = ChannelCreate(0);
    if (chid < 0)
        return 1;
    int coid = ConnectAttach(0, 0, chid, 0, 0);
    if (coid < 0) {
        ChannelDestroy(chid);
        return 1;
    }

    if (MsgSendPulse(coid, -1, 0x11, 0x22) != 0)
        g_failed = 1;

    unsigned char small[8];
    errno = 0;
    if (MsgReceivePulse(chid, small, sizeof small, NULL) != -1)
        g_failed = 1;
    if (errno != EFAULT)
        g_failed = 1;

    // The pulse is gone, so this call must block until the timeout fires.
    struct sigevent unblock;
    memset(&unblock, 0, sizeof unblock);
    SIGEV_UNBLOCK_INIT(&unblock);

    uint64_t ntime = 10ull * 1000ull * 1000ull;
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, &unblock, &ntime,
                     NULL) != 0)
        g_failed = 1;

    struct _pulse pulse;
    memset(&pulse, 0, sizeof pulse);
    if (MsgReceivePulse(chid, &pulse, sizeof pulse, NULL) != -1)
        g_failed = 1;

    ConnectDetach(coid);
    ChannelDestroy(chid);
    return g_failed ? 1 : 0;
}
