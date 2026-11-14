// SPDX-License-Identifier: Apache-2.0

#include "kernel/oracle.h"

#include <stdio.h>

#include "kernel/objects.h"

static const char *state_name(uint8_t state)
{
    switch (state) {
    case PERMUTE_THREAD_REPLY:
        return "REPLY";
    case PERMUTE_THREAD_JOIN:
        return "JOIN";
    case PERMUTE_THREAD_SEND:
        return "SEND";
    case PERMUTE_THREAD_RECEIVE:
        return "RECEIVE";
    case PERMUTE_THREAD_NANOSLEEP:
        return "NANOSLEEP";
    case PERMUTE_THREAD_MUTEX:
        return "MUTEX";
    case PERMUTE_THREAD_COND:
        return "COND";
    case PERMUTE_THREAD_SEM:
        return "SEM";
    default:
        return "BLOCKED";
    }
}

// The thread this one cannot progress without, or 0 when any counterpart will
// do. Only REPLY and JOIN name a specific peer; a sender waits for a receiver,
// and modelling that as an edge to an arbitrary thread would invent a cycle.
static uint32_t peer_of(permute_kernel_t *k, const permute_thread_t *t,
                        int32_t *rcvid)
{
    *rcvid = 0;

    if (t->state == PERMUTE_THREAD_REPLY) {
        permute_message_t *m = permute_message_get(k, t->wmsg);
        *rcvid = t->wrcvid;
        return m != NULL ? m->receiver_tid : 0;
    }

    if (t->state == PERMUTE_THREAD_JOIN) {
        permute_thread_t *target = permute_thread_get(k, t->wobj);
        return target != NULL ? target->tid : 0;
    }

    return 0;
}

int permute_oracle_deadlock(permute_kernel_t *k, permute_oracle_cycle_t *cycle)
{
    if (k == NULL || cycle == NULL)
        return 0;

    cycle->len = 0;

    for (uint32_t slot = 0; slot < k->threads.capacity; slot++) {
        if (!k->threads.in_use[slot])
            continue;

        permute_thread_t *start = &k->thread_pool[slot];
        if (start->state == PERMUTE_THREAD_DEAD ||
            start->state == PERMUTE_THREAD_ZOMBIE)
            continue;

        int32_t first_rcvid = 0;
        if (peer_of(k, start, &first_rcvid) == 0)
            continue;

        uint32_t path[PERMUTE_ORACLE_MAX_CYCLE];
        int32_t rcvid[PERMUTE_ORACLE_MAX_CYCLE];
        uint8_t state[PERMUTE_ORACLE_MAX_CYCLE];
        uint32_t length = 0;

        path[length] = start->tid;
        state[length] = (uint8_t)start->state;
        rcvid[length] = first_rcvid;
        length++;

        int32_t ignored = 0;
        uint32_t node = peer_of(k, start, &ignored);
        while (node != 0 && length < PERMUTE_ORACLE_MAX_CYCLE) {
            uint32_t index = length;
            for (uint32_t i = 0; i < length; i++) {
                if (path[i] == node) {
                    index = i;
                    break;
                }
            }

            if (index < length) {
                // The walk returned to a node on the path: that is the cycle.
                for (uint32_t i = index; i < length; i++) {
                    cycle->tid[cycle->len] = path[i];
                    cycle->state[cycle->len] = state[i];
                    cycle->rcvid[cycle->len] = rcvid[i];
                    cycle->len++;
                }
                return cycle->len > 0;
            }

            permute_thread_t *t =
                permute_thread_by_global_tid(k, node);
            if (t == NULL)
                break;

            int32_t edge_rcvid = 0;
            uint32_t next = peer_of(k, t, &edge_rcvid);
            if (next == 0)
                break;

            path[length] = node;
            state[length] = (uint8_t)t->state;
            rcvid[length] = edge_rcvid;
            length++;
            node = next;
        }
    }

    return 0;
}

int permute_oracle_format_cycle(const permute_oracle_cycle_t *cycle, char *buf,
                                size_t len)
{
    if (cycle == NULL || cycle->len == 0) {
        if (len > 0)
            buf[0] = '\0';
        return 0;
    }

    int written = 0;
    for (uint32_t i = 0; i <= cycle->len; i++) {
        uint32_t index = i % cycle->len;
        const char *sep = i == 0 ? "" : " -> ";

        int n;
        if (cycle->state[index] == PERMUTE_THREAD_REPLY) {
            n = snprintf(buf + written,
                         written < (int)len ? len - (size_t)written : 0,
                         "%sT%u(REPLY %d)", sep, cycle->tid[index],
                         cycle->rcvid[index]);
        } else {
            n = snprintf(buf + written,
                         written < (int)len ? len - (size_t)written : 0,
                         "%sT%u(%s)", sep, cycle->tid[index],
                         state_name(cycle->state[index]));
        }
        if (n < 0)
            return n;
        written += n;
    }

    return written;
}
