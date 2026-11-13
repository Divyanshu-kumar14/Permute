// SPDX-License-Identifier: Apache-2.0
//
// Shim routing: identity ordinals, virtual time, and loud failure when no
// kernel table is installed.
//
// The test installs a fake operation table through the hypercall page, which
// is exactly the mechanism the real host uses.

#define _GNU_SOURCE

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "abi/permute_iface.h"
#include "guest/hypercall.h"
#include "harness.h"

static pid_t fake_getpid(void)
{
    return 4242;
}

static pid_t fake_gettid(void)
{
    return 77;
}

static uint64_t fake_token(void)
{
    return 0x1122334455667788ull;
}

static int fake_clock(int clock_id, struct timespec *ts)
{
    (void)clock_id;
    ts->tv_sec = 1234;
    ts->tv_nsec = 5678;
    return 0;
}

static int fake_nanosleep(const struct timespec *req, struct timespec *rem)
{
    (void)req;
    (void)rem;
    return 0;
}

static uint64_t fake_rand_seed(void)
{
    return 42;
}

static int fake_channel_destroy(int chid) { return chid >= 0 ? 0 : -1; }
static int fake_connect_detach(int coid) { return coid >= 0 ? 0 : -1; }
static int fake_msg_error(rcvid_t rcvid, int error)
{
    (void)rcvid;
    (void)error;
    return 0;
}
static int fake_msg_send_pulse(int coid, int priority, int code, long value)
{
    (void)coid;
    (void)priority;
    (void)code;
    (void)value;
    return 0;
}
static int fake_msg_receive_pulse(int chid, void *pulse, size_t bytes,
                                  struct _msg_info *info)
{
    (void)chid;
    (void)pulse;
    (void)bytes;
    (void)info;
    return 0;
}
static int fake_msg_register_event(struct sigevent *event, int coid)
{
    (void)event;
    (void)coid;
    return 0;
}
static int fake_msg_deliver_event(rcvid_t rcvid, const struct sigevent *event)
{
    (void)rcvid;
    (void)event;
    return 0;
}
static int fake_msg_verify_event(rcvid_t rcvid, const struct sigevent *event)
{
    (void)rcvid;
    (void)event;
    return 0;
}
static int fake_timer_timeout(int clock_id, uint32_t flags,
                              const struct sigevent *event,
                              const uint64_t *ntime, uint64_t *otime)
{
    (void)clock_id;
    (void)flags;
    (void)event;
    (void)ntime;
    (void)otime;
    return 0;
}
static int fake_sched_set(pid_t pid, int tid, int policy,
                          const struct sched_param *param)
{
    (void)pid;
    (void)tid;
    (void)policy;
    (void)param;
    return 0;
}
static int fake_sched_get(pid_t pid, int tid, struct sched_param *param)
{
    (void)pid;
    (void)tid;
    if (param != NULL)
        param->sched_priority = 17;
    return 0;
}
static int fake_sched_yield(void) { return 0; }

static int fake_channel_create(unsigned flags)
{
    return (int)flags + 1;
}

static int fake_connect_attach(uint32_t reserved, pid_t pid, int chid,
                               unsigned index, int flags)
{
    (void)reserved;
    (void)pid;
    (void)chid;
    (void)index;
    return flags + 1;
}

static long fake_msg_send(int coid, const void *smsg, size_t sbytes,
                          void *rmsg, size_t rbytes)
{
    (void)smsg;
    (void)sbytes;
    (void)rmsg;
    (void)rbytes;
    return coid + 1;
}

static rcvid_t fake_msg_receive(int chid, void *msg, size_t bytes,
                                struct _msg_info *info)
{
    (void)msg;
    (void)bytes;
    (void)info;
    return chid + 1;
}

static int fake_msg_reply(rcvid_t rcvid, long status, const void *msg,
                          size_t bytes)
{
    (void)status;
    (void)msg;
    (void)bytes;
    return rcvid + 1;
}

static const permute_kernel_ops_t g_ops = {
    .channel_create = fake_channel_create,
    .connect_attach = fake_connect_attach,
    .msg_send = fake_msg_send,
    .msg_receive = fake_msg_receive,
    .msg_reply = fake_msg_reply,
    .getpid = fake_getpid,
    .gettid = fake_gettid,
    .thread_token = fake_token,
    .clock_gettime = fake_clock,
    .nanosleep = fake_nanosleep,
    .rand_seed = fake_rand_seed,
    .channel_destroy = fake_channel_destroy,
    .connect_detach = fake_connect_detach,
    .msg_error = fake_msg_error,
    .msg_send_pulse = fake_msg_send_pulse,
    .msg_receive_pulse = fake_msg_receive_pulse,
    .msg_register_event = fake_msg_register_event,
    .msg_deliver_event = fake_msg_deliver_event,
    .msg_verify_event = fake_msg_verify_event,
    .timer_timeout = fake_timer_timeout,
    .sched_set = fake_sched_set,
    .sched_get = fake_sched_get,
    .sched_yield = fake_sched_yield,
};

int main(void)
{
    uintptr_t base = permute_hypercall_init();
    // ASan reserves the fixed candidates, so the page can be unavailable in
    // the sanitizer build. The fallback install path is a supported mode and
    // is exercised there; the normal build exercises the page.
    printf("hypercall page: %s\n", base != 0 ? "fixed" : "fallback");
    CHECK_EQ(permute_ops_install(&g_ops), 0);

    // FR-4.9: ordinals, not host values or addresses.
    CHECK_EQ(getpid(), 4242);
    CHECK_EQ(gettid(), 77);

    pthread_t self = pthread_self();
    CHECK_EQ((uint64_t)self, 0x1122334455667788ull);
    CHECK(pthread_equal(self, (pthread_t)0x1122334455667788ull));
    CHECK(!pthread_equal(self, (pthread_t)1));

    // FR-4.8: every time source reads the virtual clock.
    struct timespec ts;
    memset(&ts, 0, sizeof ts);
    CHECK_EQ(clock_gettime(CLOCK_MONOTONIC, &ts), 0);
    CHECK_EQ(ts.tv_sec, 1234);
    CHECK_EQ(ts.tv_nsec, 5678);

    time_t t = time(NULL);
    CHECK_EQ(t, (time_t)1234);
    time_t out = 0;
    CHECK_EQ(time(&out), (time_t)1234);
    CHECK_EQ(out, (time_t)1234);

    struct timeval tv;
    memset(&tv, 0, sizeof tv);
    CHECK_EQ(gettimeofday(&tv, NULL), 0);
    CHECK_EQ(tv.tv_sec, 1234);
    CHECK_EQ(tv.tv_usec, 5); // 5678 ns

    CHECK_EQ(nanosleep(&ts, NULL), 0);

    // FR-4.11: the generator is a pure function of the process seed and the
    // caller's srand argument.
    srand(7);
    int r1 = rand();
    srand(7);
    CHECK_EQ(rand(), r1);
    srand(8);
    CHECK(rand() != r1);

    // rand and random share one stream, as POSIX permits.
    srand(9);
    long q1 = random();
    srand(9);
    CHECK_EQ((long)rand(), q1);

    // With no table, every shim fails loudly rather than falling back.
    // The rest of the surface, on the installed table.
    struct sigevent event;
    memset(&event, 0, sizeof event);
    struct _pulse pulse;
    memset(&pulse, 0, sizeof pulse);
    struct sched_param sched;
    memset(&sched, 0, sizeof sched);
    uint64_t ntime = 1000;
    uint64_t otime = 0;

    CHECK_EQ(ChannelDestroy(1), 0);
    CHECK_EQ(ConnectDetach(1), 0);
    CHECK_EQ(MsgError(1, EINVAL), 0);
    CHECK_EQ(MsgSendPulse(1, 5, 0x42, 7), 0);
    CHECK_EQ(MsgReceivePulse(1, &pulse, sizeof pulse, NULL), 0);
    CHECK_EQ(MsgRegisterEvent(&event, 1), 0);
    CHECK_EQ(MsgDeliverEvent(1, &event), 0);
    CHECK_EQ(MsgVerifyEvent(1, &event), 0);
    CHECK_EQ(TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND, &event, &ntime,
                          &otime),
             0);
    CHECK_EQ(SchedSet(0, 0, SCHED_FIFO, &sched), 0);
    CHECK_EQ(SchedGet(0, 0, &sched), 0);
    CHECK_EQ(sched.sched_priority, 17);
    CHECK_EQ(SchedYield(), 0);

    CHECK_EQ(permute_ops_install(NULL), 0);

    errno = 0;
    CHECK_EQ(getpid(), -1);
    CHECK_EQ(errno, ENOSYS);

    errno = 0;
    CHECK_EQ(gettid(), -1);
    CHECK_EQ(errno, ENOSYS);

    errno = 0;
    // glibc marks pthread_self const, so a direct second call is folded into
    // the first. The volatile pointer forces the call we want to test.
    pthread_t (*volatile self_fn)(void) = pthread_self;
    CHECK_EQ((uint64_t)self_fn(), 0);
    CHECK_EQ(errno, ENOSYS);

    errno = 0;
    CHECK_EQ(clock_gettime(CLOCK_MONOTONIC, &ts), -1);
    CHECK_EQ(errno, ENOSYS);

    errno = 0;
    CHECK_EQ(nanosleep(&ts, NULL), -1);
    CHECK_EQ(errno, ENOSYS);

    // Every new entry point must fail loudly with no table as well.
    errno = 0;
    CHECK_EQ(ChannelDestroy(1), -1);
    CHECK_EQ(errno, ENOSYS);
    errno = 0;
    CHECK_EQ(ConnectDetach(1), -1);
    CHECK_EQ(errno, ENOSYS);
    errno = 0;
    CHECK_EQ(MsgError(1, EINVAL), -1);
    CHECK_EQ(errno, ENOSYS);
    errno = 0;
    CHECK_EQ(MsgSendPulse(1, 5, 0x42, 7), -1);
    CHECK_EQ(errno, ENOSYS);
    errno = 0;
    CHECK_EQ(MsgReceivePulse(1, &pulse, sizeof pulse, NULL), -1);
    CHECK_EQ(errno, ENOSYS);
    errno = 0;
    CHECK_EQ(MsgRegisterEvent(&event, 1), -1);
    CHECK_EQ(errno, ENOSYS);
    errno = 0;
    CHECK_EQ(MsgDeliverEvent(1, &event), -1);
    CHECK_EQ(errno, ENOSYS);
    errno = 0;
    CHECK_EQ(MsgVerifyEvent(1, &event), -1);
    CHECK_EQ(errno, ENOSYS);
    errno = 0;
    CHECK_EQ(TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND, &event, &ntime,
                          &otime),
             -1);
    CHECK_EQ(errno, ENOSYS);
    errno = 0;
    CHECK_EQ(SchedSet(0, 0, SCHED_FIFO, &sched), -1);
    CHECK_EQ(errno, ENOSYS);
    errno = 0;
    CHECK_EQ(SchedGet(0, 0, &sched), -1);
    CHECK_EQ(errno, ENOSYS);
    errno = 0;
    CHECK_EQ(SchedYield(), -1);
    CHECK_EQ(errno, ENOSYS);

    return TEST_RESULT("test_shim");
}
