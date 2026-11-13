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

    return TEST_RESULT("test_shim");
}
