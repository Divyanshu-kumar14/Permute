// SPDX-License-Identifier: Apache-2.0
//
// Day-1 spike: prove that interposition actually happens.
//
// What is proven, in order of strength:
//   1. A call to an IPC symbol binds to the archive, directly and through a
//      function pointer (the linker cannot be fooled by a pointer).
//   2. A symbol libc also defines (`write`) binds to our archive, which is the
//      real precedence question (MsgSend has no libc competitor on Linux).
//   3. With no kernel installed the shim fails loudly (-1/ENOSYS) instead of
//      pretending to work.
//   4. Informational: whether libc-internal callers (stdio) reach the probe.
//      This is a host baseline for the target-side question; it is NOT the
//      target answer and must not be read as one.

#define _GNU_SOURCE
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

#include "abi/permute_iface.h"
#include "guest/hypercall.h"

extern _Atomic unsigned long permute_probe_write_calls;

static unsigned long n_chan, n_conn, n_send, n_recv, n_reply;

static int mock_channel_create(unsigned flags)
{
    (void)flags;
    n_chan++;
    return 7;
}

static int mock_connect_attach(uint32_t reserved, pid_t pid, int chid,
                               unsigned index, int flags)
{
    (void)reserved; (void)pid; (void)chid; (void)index; (void)flags;
    n_conn++;
    return 9;
}

static long mock_msg_send(int coid, const void *smsg, size_t sbytes,
                          void *rmsg, size_t rbytes)
{
    (void)coid; (void)smsg; (void)sbytes; (void)rmsg; (void)rbytes;
    n_send++;
    return 0x1234;
}

static rcvid_t mock_msg_receive(int chid, void *msg, size_t bytes,
                                struct _msg_info *info)
{
    (void)chid; (void)msg; (void)bytes; (void)info;
    n_recv++;
    return 42;
}

static int mock_msg_reply(rcvid_t rcvid, long status, const void *msg,
                          size_t bytes)
{
    (void)rcvid; (void)status; (void)msg; (void)bytes;
    n_reply++;
    return 0;
}

static const permute_kernel_ops_t mock_ops = {
    .channel_create = mock_channel_create,
    .connect_attach = mock_connect_attach,
    .msg_send       = mock_msg_send,
    .msg_receive    = mock_msg_receive,
    .msg_reply      = mock_msg_reply,
};

static int failures;

static void check(int condition, const char *what)
{
    printf("%s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition)
        failures++;
}

int main(void)
{
    permute_ops_install(&mock_ops);

    check(ChannelCreate(0) == 7 && n_chan == 1,
          "direct ChannelCreate reaches the shim");
    check(ConnectAttach(0, 0, 1, 0, 0) == 9 && n_conn == 1,
          "direct ConnectAttach reaches the shim");
    check(MsgSend(1, "x", 1, NULL, 0) == 0x1234 && n_send == 1,
          "direct MsgSend reaches the shim");

    long (*send_fp)(int, const void *, size_t, void *, size_t) = MsgSend;
    check(send_fp(1, "x", 1, NULL, 0) == 0x1234 && n_send == 2,
          "MsgSend through a function pointer reaches the shim");

    check(MsgReceive(1, NULL, 0, NULL) == 42 && n_recv == 1,
          "direct MsgReceive reaches the shim");
    check(MsgReply(42, 0, NULL, 0) == 0 && n_reply == 1,
          "direct MsgReply reaches the shim");

    permute_ops_install(NULL);
    errno = 0;
    check(MsgSend(1, "x", 1, NULL, 0) == -1 && errno == ENOSYS,
          "no kernel installed -> -1/ENOSYS, no silent success");
    permute_ops_install(&mock_ops);

    fflush(stdout);

    unsigned long before = atomic_load(&permute_probe_write_calls);
    ssize_t w = write(STDOUT_FILENO, "", 0);
    (void)w;
    unsigned long after = atomic_load(&permute_probe_write_calls);
    check(after == before + 1,
          "direct write() binds to the probe archive, not libc");

    fflush(stdout);
    before = atomic_load(&permute_probe_write_calls);
    printf("probe-line\n");
    fflush(stdout);
    after = atomic_load(&permute_probe_write_calls);

    int libc_internal = (after > before);
    printf("INFO  printf() reached the write probe: %s (%lu call(s))\n",
           libc_internal ? "yes" : "no", after - before);
    printf("INFO  host baseline only; the target answer decides Q1\n");

    printf("\n%s\n", failures ? "SPIKE FAILED" : "SPIKE PASSED");
    return failures ? 1 : 0;
}
