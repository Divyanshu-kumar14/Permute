// SPDX-License-Identifier: Apache-2.0
//
// The seeded scenario used by replay and by permute-run.
//
// This is not the executor. It is the smallest driver that exercises the
// kernel state machine end to end from a seed: two clients, a server, 200
// iterations of send/receive/reply plus seeded pulses. Every choice that
// could vary comes from the decision stream or the virtual clock, and the
// trace carries no wall clock, addresses, or host information.
//
// Internal failure exits rather than returning part-way: the caller is a
// single-seed process whose only job is this run.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/scenario.h"

#include "kernel/decisions.h"
#include "kernel/ipc.h"
#include "kernel/objects.h"
#include "kernel/trace.h"
#include "kernel/vclock.h"

#define ITERATIONS 200
#define CLIENTS 2

static permute_kernel_t K;
static permute_trace_t T;
static uint64_t g_seq;

static void die(const char *what)
{
    fprintf(stderr, "scenario: %s\n", what);
    exit(7);
}

static void emit(uint8_t kind, uint64_t src, uint64_t dst, uint32_t obj,
                 uint32_t aux, const void *blob, uint16_t blob_len)
{
    g_seq++;
    if (permute_trace_emit(&T, kind, g_seq, K.now_ns, (uint16_t)src,
                           (uint16_t)dst, obj, aux, blob, blob_len, NULL) != 0)
        die("trace emit failed");
}

int permute_scenario_run(uint64_t seed, const char *path,
                          uint64_t *digest_out, uint64_t *records_out)
{
    uint8_t build_id[32];
    uint8_t run_id[16];
    for (int i = 0; i < 32; i++)
        build_id[i] = (uint8_t)i;
    for (int i = 0; i < 16; i++)
        run_id[i] = (uint8_t)(0x10 + i);

    permute_kernel_init(&K, seed);

    if (permute_trace_open(&T, path, 0xC0FFEEull, build_id, run_id, 0) != 0)
        die("cannot open trace");

    permute_handle_t srv, srv_t;
    int srv_chid = -1;
    permute_handle_t ch;
    if (permute_process_create(&K, "srv", &srv) != 0 ||
        permute_thread_create(&K, srv, 10, PERMUTE_SCHED_FIFO, &srv_t) != 0 ||
        permute_channel_create(&K, srv, 0, &srv_chid, &ch) != 0)
        die("server setup failed");

    permute_handle_t cli_proc[CLIENTS], cli_t[CLIENTS];
    int cli_coid[CLIENTS];
    for (int i = 0; i < CLIENTS; i++) {
        permute_handle_t conn;
        if (permute_process_create(&K, "cli", &cli_proc[i]) != 0 ||
            permute_thread_create(&K, cli_proc[i], 10, PERMUTE_SCHED_FIFO,
                                  &cli_t[i]) != 0 ||
            permute_connect_attach(&K, cli_proc[i], srv, srv_chid, 0,
                                   &cli_coid[i], &conn) != 0)
            die("client setup failed");
    }

    permute_stream_t ds;
    permute_stream_init(&ds, seed);

    uint32_t srv_tid = permute_thread_get(&K, srv_t)->tid;
    uint32_t cli_tid[CLIENTS];
    for (int i = 0; i < CLIENTS; i++)
        cli_tid[i] = permute_thread_get(&K, cli_t[i])->tid;

    uint8_t payload[64];
    uint8_t received[64];
    permute_result_t r;
    permute_msg_info_t info;

    for (int i = 0; i < ITERATIONS; i++) {
        int c = (int)(permute_stream_next(&ds, PERMUTE_D_SCHED) % CLIENTS);
        uint32_t len =
            1 + (uint32_t)(permute_stream_next(&ds, PERMUTE_D_STIMULUS) % 64);

        for (uint32_t j = 0; j < len; j++)
            payload[j] = (uint8_t)permute_stream_next(&ds, PERMUTE_D_RECV_ORDER);

        // The clock moves by a decision-derived amount, never by wall time.
        uint64_t dt = 1 + permute_stream_next(&ds, PERMUTE_D_CPUJITTER) % 1000;
        if (permute_vclock_advance(&K, K.now_ns + dt) != 0)
            die("clock advance failed");

        if (permute_ipc_send(&K, cli_t[c], cli_coid[c], payload, len,
                             NULL, 0, &r) != PERMUTE_BLOCKED)
            die("send did not block");

        if (permute_ipc_receive(&K, srv_t, srv_chid, received, sizeof received,
                                &info, &r) != PERMUTE_CONTINUE)
            die("receive did not complete");

        emit(PERMUTE_TR_SEND, cli_tid[c], srv_tid, (uint32_t)cli_coid[c], len,
             payload, (uint16_t)len);
        emit(PERMUTE_TR_RECV, srv_tid, cli_tid[c], (uint32_t)r.rcvid,
             info.msglen, received, (uint16_t)r.bytes);
        emit(PERMUTE_TR_REPLY, srv_tid, cli_tid[c], (uint32_t)r.rcvid, 0, NULL,
             0);

        if (permute_ipc_reply(&K, srv_t, r.rcvid, 0, NULL, 0, &r) !=
            PERMUTE_CONTINUE)
            die("reply failed");

        // A pulse every fourth iteration, decided by the stream.
        if ((permute_stream_next(&ds, PERMUTE_D_QUANTUM) & 3u) == 0) {
            uint8_t value[8];
            int32_t code =
                1 + (int32_t)(permute_stream_next(&ds, PERMUTE_D_TIMER_TIE) % 100);
            uint64_t v = permute_stream_next(&ds, PERMUTE_D_FAULT);
            for (int b = 0; b < 8; b++)
                value[b] = (uint8_t)(v >> (8 * b));

            if (permute_ipc_send_pulse(&K, cli_t[c], cli_coid[c], -1, code, v,
                                       &r) != PERMUTE_CONTINUE)
                die("pulse send failed");

            if (permute_ipc_receive(&K, srv_t, srv_chid, NULL, 0, NULL, &r) !=
                PERMUTE_CONTINUE)
                die("pulse receive failed");

            emit(PERMUTE_TR_PULSE_SEND, cli_tid[c], srv_tid,
                 (uint32_t)cli_coid[c], (uint32_t)code, value, 8);
            emit(PERMUTE_TR_PULSE_RECV, srv_tid, cli_tid[c],
                 (uint32_t)r.pulse.code, (uint32_t)r.pulse.priority, value, 8);
        }
    }

    uint64_t digest = 0;
    if (permute_trace_close(&T, &digest) != 0)
        die("trace close failed");

    if (digest_out != NULL)
        *digest_out = digest;
    if (records_out != NULL)
        *records_out = g_seq;
    return 0;
}
