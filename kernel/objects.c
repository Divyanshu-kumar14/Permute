// SPDX-License-Identifier: Apache-2.0

#include "kernel/objects.h"

#include <errno.h>
#include <string.h>

#include "kernel/blob.h"
#include "kernel/ipc_internal.h"

// Backing store for payload blobs. One kernel per process (IMP-13), so one
// backing store; permute_kernel_init() rebinds it on every reset.
static _Alignas(8) uint8_t g_blob_storage[PERMUTE_BLOB_ARENA_SIZE];

static void thread_teardown(permute_kernel_t *k, uint32_t slot);
static void channel_drain(permute_kernel_t *k, permute_channel_t *ch);

// ---------------------------------------------------------------------------
// Handle tables

void permute_table_init(permute_table_t *t, uint32_t capacity)
{
    if (capacity > PERMUTE_MAX_SLOTS)
        capacity = PERMUTE_MAX_SLOTS;

    memset(t, 0, sizeof *t);
    t->capacity = capacity;
    t->free_head = 0;
    t->live = 0;

    for (uint32_t i = 0; i < capacity; i++) {
        t->next_free[i] = (i + 1u < capacity) ? i + 1u : UINT32_MAX;
        t->generation[i] = 0;
        t->in_use[i] = 0;
    }
}

int permute_table_alloc(permute_table_t *t, uint32_t *slot, uint32_t *gen)
{
    if (t->free_head == UINT32_MAX)
        return -EMFILE;

    uint32_t s = t->free_head;
    t->free_head = t->next_free[s];
    t->in_use[s] = 1;
    t->live++;

    *slot = s;
    *gen = t->generation[s];
    return 0;
}

void permute_table_free(permute_table_t *t, uint32_t slot)
{
    if (slot >= t->capacity || !t->in_use[slot])
        return;

    t->in_use[slot] = 0;
    t->generation[slot]++; // wraps at 2^32; unreachable in a realistic run
    t->next_free[slot] = t->free_head;
    t->free_head = slot;
    t->live--;
}

int permute_table_valid(const permute_table_t *t, permute_handle_t h)
{
    if (h == PERMUTE_HANDLE_NONE)
        return 0;

    uint32_t slot = PERMUTE_HANDLE_SLOT(h);
    if (slot >= t->capacity || !t->in_use[slot])
        return 0;

    return t->generation[slot] == PERMUTE_HANDLE_GEN(h);
}

// ---------------------------------------------------------------------------
// Kernel

void permute_kernel_init(permute_kernel_t *k, uint64_t seed)
{
    memset(k, 0, sizeof *k);

    permute_table_init(&k->processes, PERMUTE_MAX_PROCESSES);
    permute_table_init(&k->threads, PERMUTE_MAX_THREADS);
    permute_table_init(&k->channels, PERMUTE_MAX_CHANNELS);
    permute_table_init(&k->connections, PERMUTE_MAX_CONNECTIONS);
    permute_table_init(&k->messages, PERMUTE_MAX_MESSAGES);
    permute_table_init(&k->pulses, PERMUTE_MAX_PULSES);
    permute_table_init(&k->registrations, PERMUTE_MAX_REGISTRATIONS);

    permute_blob_init(&k->blobs, g_blob_storage, sizeof g_blob_storage);

    k->next_pid = 1; // pid 0 is the kernel
    k->next_tid = 1;
    k->next_registration = 1; // handle 0 is never valid
    k->sequence = 1;
    k->seed = seed;
}

void permute_kernel_destroy(permute_kernel_t *k)
{
    permute_blob_init(&k->blobs, g_blob_storage, sizeof g_blob_storage);
}

// ---------------------------------------------------------------------------
// Processes

permute_process_t *permute_process_get(permute_kernel_t *k, permute_handle_t h)
{
    if (!permute_table_valid(&k->processes, h))
        return NULL;
    return &k->process_pool[PERMUTE_HANDLE_SLOT(h)];
}

permute_process_t *permute_process_by_pid(permute_kernel_t *k, uint32_t pid)
{
    if (pid == 0)
        return NULL; // the kernel

    for (uint32_t i = 0; i < k->processes.capacity; i++) {
        if (k->processes.in_use[i] && k->process_pool[i].pid == pid)
            return &k->process_pool[i];
    }
    return NULL;
}

permute_handle_t permute_process_handle_by_pid(permute_kernel_t *k,
                                               uint32_t pid)
{
    for (uint32_t i = 0; i < k->processes.capacity; i++) {
        if (k->processes.in_use[i] && k->process_pool[i].pid == pid)
            return PERMUTE_HANDLE_MAKE(i, k->processes.generation[i]);
    }
    return PERMUTE_HANDLE_NONE;
}

int permute_process_create(permute_kernel_t *k, const char *name,
                           permute_handle_t *out)
{
    uint32_t slot, gen;
    int rc = permute_table_alloc(&k->processes, &slot, &gen);
    if (rc != 0)
        return rc;

    permute_process_t *p = &k->process_pool[slot];
    memset(p, 0, sizeof *p);

    p->pid = k->next_pid++;
    if (name != NULL) {
        strncpy(p->name, name, PERMUTE_NAME_MAX - 1);
        p->name[PERMUTE_NAME_MAX - 1] = '\0';
    }

    for (uint32_t i = 0; i < PERMUTE_MAX_CH; i++)
        p->channels[i] = PERMUTE_HANDLE_NONE;
    for (uint32_t i = 0; i < PERMUTE_MAX_CO; i++)
        p->connections[i] = PERMUTE_HANDLE_NONE;

    p->prio_ceiling = PERMUTE_DEFAULT_PRIO_CEIL;
    p->abilities = PERMUTE_AID_PRIORITY;
    permute_stream_init(&p->decisions, k->seed ^ (uint64_t)p->pid);

    *out = PERMUTE_HANDLE_MAKE(slot, gen);
    return 0;
}

int permute_process_destroy(permute_kernel_t *k, permute_handle_t h)
{
    permute_process_t *p = permute_process_get(k, h);
    if (p == NULL)
        return -ESRCH;

    uint32_t pid = p->pid;

    // Deterministic teardown order: threads, own channels, client connections,
    // registrations, in-flight messages, process slot. Each step is complete
    // before the next starts, so nothing observes a half-dead process.

    for (uint32_t i = 0; i < k->threads.capacity; i++) {
        if (k->threads.in_use[i] && k->thread_pool[i].pid == pid)
            thread_teardown(k, i);
    }

    for (uint32_t i = 0; i < k->channels.capacity; i++) {
        if (!k->channels.in_use[i] || k->channel_pool[i].owner_pid != pid)
            continue;
        channel_drain(k, &k->channel_pool[i]);
        permute_table_free(&k->channels, i);
    }

    for (uint32_t i = 0; i < k->connections.capacity; i++) {
        if (!k->connections.in_use[i])
            continue;
        permute_connection_t *c = &k->connection_pool[i];
        if (c->client_pid != pid)
            continue;

        permute_handle_t chan = c->channel;
        int32_t coid = c->coid;

        if (permute_table_valid(&k->channels, chan))
            k->channel_pool[PERMUTE_HANDLE_SLOT(chan)].conn_refs--;
        permute_table_free(&k->connections, i);

        // A dying client is detached like any other; servers that asked for
        // disconnect notification still get it.
        permute_pulse_on_detach(k, chan, coid, pid);
    }

    for (uint32_t i = 0; i < k->registrations.capacity; i++) {
        if (!k->registrations.in_use[i])
            continue;
        if (k->registration_pool[i].owner_pid == pid)
            permute_table_free(&k->registrations, i);
    }

    for (uint32_t i = 0; i < k->messages.capacity; i++) {
        if (!k->messages.in_use[i])
            continue;
        permute_message_t *m = &k->message_pool[i];
        if (m->src_pid != pid)
            continue;

        if (m->in_receive_queue) {
            permute_channel_t *ch = permute_channel_get(k, m->channel);
            if (ch != NULL)
                permute_queue_remove(
                    &ch->recvq,
                    PERMUTE_HANDLE_MAKE(i, k->messages.generation[i]));
        }
        permute_message_destroy(
            k, PERMUTE_HANDLE_MAKE(i, k->messages.generation[i]));
    }

    permute_table_free(&k->processes, PERMUTE_HANDLE_SLOT(h));
    return 0;
}

// ---------------------------------------------------------------------------
// Threads

permute_thread_t *permute_thread_get(permute_kernel_t *k, permute_handle_t h)
{
    if (!permute_table_valid(&k->threads, h))
        return NULL;
    return &k->thread_pool[PERMUTE_HANDLE_SLOT(h)];
}

permute_thread_t *permute_thread_by_tid(permute_kernel_t *k, uint32_t pid,
                                        uint32_t tid)
{
    for (uint32_t i = 0; i < k->threads.capacity; i++) {
        if (!k->threads.in_use[i])
            continue;
        if (k->thread_pool[i].pid == pid && k->thread_pool[i].tid == tid)
            return &k->thread_pool[i];
    }
    return NULL;
}

permute_thread_t *permute_thread_by_global_tid(permute_kernel_t *k,
                                               uint32_t tid)
{
    for (uint32_t i = 0; i < k->threads.capacity; i++) {
        if (!k->threads.in_use[i])
            continue;
        if (k->thread_pool[i].tid == tid)
            return &k->thread_pool[i];
    }
    return NULL;
}

void permute_thread_recompute_prio(permute_thread_t *t)
{
    t->eff_prio = permute_eff_prio(t);
}

void permute_thread_set_prio(permute_thread_t *t, int32_t base_prio)
{
    t->base_prio = base_prio;
    t->sticky_prio = 0; // explicit scheduling clears inherited priority
    permute_thread_recompute_prio(t);
}

int permute_thread_create(permute_kernel_t *k, permute_handle_t process,
                          int32_t base_prio, permute_policy_t policy,
                          permute_handle_t *out)
{
    permute_process_t *p = permute_process_get(k, process);
    if (p == NULL)
        return -ESRCH;

    if (base_prio < 0)
        base_prio = 0;
    if (base_prio > PERMUTE_PRIO_MAX)
        base_prio = PERMUTE_PRIO_MAX;

    uint32_t slot, gen;
    int rc = permute_table_alloc(&k->threads, &slot, &gen);
    if (rc != 0)
        return rc;

    permute_thread_t *t = &k->thread_pool[slot];
    memset(t, 0, sizeof *t);

    t->tid = k->next_tid++;
    t->pid = p->pid;
    t->process = process;
    t->base_prio = base_prio;
    t->policy = policy;
    t->state = PERMUTE_THREAD_READY;
    t->wobj = PERMUTE_HANDLE_NONE;
    t->wrcvid = 0;
    permute_thread_recompute_prio(t);

    *out = PERMUTE_HANDLE_MAKE(slot, gen);
    return 0;
}

// Tear down one thread slot. A dying thread leaves two traces that must not// outlive it: an entry in a channel's waiter queue, and a received message it
// never replied to. Leaving either behind turns a thread death into a hang in
// an unrelated process - a fault in the harness, not in the SUT.
static void thread_teardown(permute_kernel_t *k, uint32_t slot)
{
    permute_thread_t *t = &k->thread_pool[slot];
    permute_handle_t self =
        PERMUTE_HANDLE_MAKE(slot, k->threads.generation[slot]);

    if (t->state == PERMUTE_THREAD_RECEIVE && t->wobj != PERMUTE_HANDLE_NONE) {
        permute_channel_t *ch = permute_channel_get(k, t->wobj);
        if (ch != NULL)
            permute_queue_remove(&ch->recv_waiters, self);
    }

    for (uint32_t i = 0; i < k->messages.capacity; i++) {
        if (!k->messages.in_use[i])
            continue;
        permute_message_t *m = &k->message_pool[i];
        if (m->receiver_tid != t->tid || m->src_tid == t->tid)
            continue;

        // The server thread died holding the message: the client's MsgSend
        // cannot complete, so it must fail rather than block forever.
        permute_thread_t *client =
            permute_thread_by_tid(k, m->src_pid, m->src_tid);
        if (client != NULL && client->state == PERMUTE_THREAD_REPLY) {
            client->state = PERMUTE_THREAD_READY;
            client->wrcvid = 0;
            client->pending.action = PERMUTE_CONTINUE;
            client->pending.ret = -1;
            client->pending.err = ESRCH;
            client->pending.rcvid = 0;
            client->pending.bytes = 0;
        }

        permute_message_destroy(
            k, PERMUTE_HANDLE_MAKE(i, k->messages.generation[i]));
    }

    t->state = PERMUTE_THREAD_DEAD;
    t->wobj = PERMUTE_HANDLE_NONE;
    permute_table_free(&k->threads, slot);
}

int permute_thread_destroy(permute_kernel_t *k, permute_handle_t h)
{
    if (permute_thread_get(k, h) == NULL)
        return -ESRCH;
    thread_teardown(k, PERMUTE_HANDLE_SLOT(h));
    return 0;
}

// ---------------------------------------------------------------------------
// Channels

permute_channel_t *permute_channel_get(permute_kernel_t *k, permute_handle_t h)
{
    if (!permute_table_valid(&k->channels, h))
        return NULL;
    return &k->channel_pool[PERMUTE_HANDLE_SLOT(h)];
}

int permute_channel_lookup(permute_kernel_t *k, permute_handle_t process,
                           int chid, permute_handle_t *out)
{
    permute_process_t *p = permute_process_get(k, process);
    if (p == NULL)
        return -ESRCH;

    if (chid < 0 || chid >= (int)PERMUTE_MAX_CH)
        return -EINVAL;

    permute_handle_t h = p->channels[chid];
    if (!permute_table_valid(&k->channels, h))
        return -ESRCH;

    *out = h;
    return 0;
}

int permute_channel_create(permute_kernel_t *k, permute_handle_t process,
                           uint32_t flags, int *chid_out,
                           permute_handle_t *out)
{
    permute_process_t *p = permute_process_get(k, process);
    if (p == NULL)
        return -ESRCH;

    int chid = -1;
    for (uint32_t i = 0; i < PERMUTE_MAX_CH; i++) {
        if (p->channels[i] == PERMUTE_HANDLE_NONE) {
            chid = (int)i;
            break;
        }
    }
    if (chid < 0)
        return -EMFILE;

    uint32_t slot, gen;
    int rc = permute_table_alloc(&k->channels, &slot, &gen);
    if (rc != 0)
        return rc;

    permute_channel_t *c = &k->channel_pool[slot];
    memset(c, 0, sizeof *c);
    c->chid = (uint32_t)chid;
    c->owner_pid = p->pid;
    c->flags = flags;
    permute_queue_init(&c->recvq);
    permute_queue_init(&c->recv_waiters);

    permute_handle_t h = PERMUTE_HANDLE_MAKE(slot, gen);
    p->channels[chid] = h;

    *chid_out = chid;
    *out = h;
    return 0;
}

// Empty a channel's queues. Senders learn that their message will never be
// received (FR-3.10) and blocked receivers learn that the channel is gone;
// both must be woken here, because the channel structure is about to be
// recycled and nothing else knows those threads are waiting on it.
static void channel_drain(permute_kernel_t *k, permute_channel_t *ch)
{
    permute_queue_entry_t e;

    while (permute_queue_pop(&ch->recvq, &e) == 0) {
        if (e.kind == PERMUTE_Q_MESSAGE) {
            permute_message_t *m = permute_message_get(k, e.handle);
            if (m != NULL) {
                permute_thread_t *s =
                    permute_thread_by_tid(k, m->src_pid, m->src_tid);
                if (s != NULL && s->state == PERMUTE_THREAD_SEND) {
                    s->state = PERMUTE_THREAD_READY;
                    s->wobj = PERMUTE_HANDLE_NONE;
                    s->pending.action = PERMUTE_CONTINUE;
                    s->pending.ret = -1;
                    s->pending.err = ESRCH;
                    s->pending.rcvid = 0;
                    s->pending.bytes = 0;
                }
                permute_message_destroy(k, e.handle);
            }
        } else if (e.kind == PERMUTE_Q_PULSE) {
            permute_pulse_destroy(k, e.handle);
        }
    }

    while (permute_queue_pop(&ch->recv_waiters, &e) == 0) {
        permute_thread_t *t = permute_thread_get(k, e.handle);
        if (t == NULL)
            continue; // stale waiter from a dead thread; nothing to wake
        t->state = PERMUTE_THREAD_READY;
        t->wobj = PERMUTE_HANDLE_NONE;
        t->pending.action = PERMUTE_CONTINUE;
        t->pending.ret = -1;
        t->pending.err = ESRCH;
        t->pending.rcvid = 0;
        t->pending.bytes = 0;
    }
}

int permute_channel_destroy(permute_kernel_t *k, permute_handle_t process,
                            int chid)
{
    permute_process_t *p = permute_process_get(k, process);
    if (p == NULL)
        return -ESRCH;

    if (chid < 0 || chid >= (int)PERMUTE_MAX_CH)
        return -EINVAL;

    permute_handle_t h = p->channels[chid];
    if (!permute_table_valid(&k->channels, h))
        return -ESRCH;

    // Connections to this channel become dangling; the next send through one
    // of them returns ESRCH. Anything already queued or waiting is told now.
    channel_drain(k, permute_channel_get(k, h));
    p->channels[chid] = PERMUTE_HANDLE_NONE;
    permute_table_free(&k->channels, PERMUTE_HANDLE_SLOT(h));
    return 0;
}

// ---------------------------------------------------------------------------
// Connections

permute_connection_t *permute_connection_get(permute_kernel_t *k,
                                             permute_handle_t h)
{
    if (!permute_table_valid(&k->connections, h))
        return NULL;
    return &k->connection_pool[PERMUTE_HANDLE_SLOT(h)];
}

int permute_connection_lookup(permute_kernel_t *k, permute_handle_t process,
                              int coid, permute_handle_t *out)
{
    permute_process_t *p = permute_process_get(k, process);
    if (p == NULL)
        return -ESRCH;

    if (coid < 0 || coid >= (int)PERMUTE_MAX_CO)
        return -EINVAL;

    permute_handle_t h = p->connections[coid];
    if (!permute_table_valid(&k->connections, h))
        return -ESRCH;

    *out = h;
    return 0;
}

// Resolve anything usable as a coid to the channel it addresses.
//
// A negative coid is a connection to the caller's own process: `coid = -5`
// addresses channel 5 of the calling process. That is documented for
// MsgSendPulse and is modelled for every entry point that takes a coid, so a
// thread can signal itself without building a connection first.
int permute_coid_to_channel(permute_kernel_t *k, permute_handle_t process,
                            int coid, permute_handle_t *chan_out,
                            int32_t *scoid_out)
{
    if (coid < 0) {
        int rc = permute_channel_lookup(k, process, -coid, chan_out);
        if (rc != 0)
            return rc;
        if (scoid_out != NULL)
            *scoid_out = 0; // no connection exists for a local pulse
        return 0;
    }

    permute_handle_t connh;
    int rc = permute_connection_lookup(k, process, coid, &connh);
    if (rc != 0)
        return rc;

    permute_connection_t *c = permute_connection_get(k, connh);
    if (c == NULL || !permute_table_valid(&k->channels, c->channel))
        return -ESRCH; // the connection outlived its channel

    *chan_out = c->channel;
    if (scoid_out != NULL)
        *scoid_out = coid;
    return 0;
}

int permute_connect_attach(permute_kernel_t *k, permute_handle_t client,
                           permute_handle_t server, int chid, uint32_t flags,
                           int *coid_out, permute_handle_t *out)
{
    permute_process_t *cp = permute_process_get(k, client);
    if (cp == NULL)
        return -ESRCH;

    permute_handle_t channel;
    int rc = permute_channel_lookup(k, server, chid, &channel);
    if (rc != 0)
        return rc;

    int coid = -1;
    for (uint32_t i = 0; i < PERMUTE_MAX_CO; i++) {
        if (cp->connections[i] == PERMUTE_HANDLE_NONE) {
            coid = (int)i;
            break;
        }
    }
    if (coid < 0)
        return -EMFILE;

    uint32_t slot, gen;
    rc = permute_table_alloc(&k->connections, &slot, &gen);
    if (rc != 0)
        return rc;

    permute_process_t *sp = permute_process_get(k, server);
    permute_connection_t *c = &k->connection_pool[slot];
    memset(c, 0, sizeof *c);
    c->coid = coid;
    c->client_pid = cp->pid;
    c->server_pid = sp->pid;
    c->channel = channel;
    c->flags = flags;
    c->refs = 1;

    k->channel_pool[PERMUTE_HANDLE_SLOT(channel)].conn_refs++;

    permute_handle_t h = PERMUTE_HANDLE_MAKE(slot, gen);
    cp->connections[coid] = h;

    *coid_out = coid;
    *out = h;
    return 0;
}

int permute_connect_detach(permute_kernel_t *k, permute_handle_t client,
                           int coid)
{
    permute_process_t *cp = permute_process_get(k, client);
    if (cp == NULL)
        return -ESRCH;

    if (coid < 0 || coid >= (int)PERMUTE_MAX_CO)
        return -EINVAL;

    permute_handle_t h = cp->connections[coid];
    if (!permute_table_valid(&k->connections, h))
        return -ESRCH;

    permute_connection_t *c = &k->connection_pool[PERMUTE_HANDLE_SLOT(h)];
    permute_handle_t chan = c->channel;
    uint32_t client_pid = c->client_pid;
    int32_t detached_coid = c->coid;

    if (permute_table_valid(&k->channels, chan))
        k->channel_pool[PERMUTE_HANDLE_SLOT(chan)].conn_refs--;

    cp->connections[coid] = PERMUTE_HANDLE_NONE;
    permute_table_free(&k->connections, PERMUTE_HANDLE_SLOT(h));

    // Emitted after the row is gone so the "last connection from this client"
    // test sees the post-detach state.
    permute_pulse_on_detach(k, chan, detached_coid, client_pid);
    return 0;
}

// ---------------------------------------------------------------------------
// Messages

permute_message_t *permute_message_get(permute_kernel_t *k, permute_handle_t h)
{
    if (!permute_table_valid(&k->messages, h))
        return NULL;
    return &k->message_pool[PERMUTE_HANDLE_SLOT(h)];
}

int permute_message_create(permute_kernel_t *k, permute_handle_t sender,
                           uint32_t sender_tid, uint32_t dst_pid,
                           int32_t send_prio, const void *payload,
                           uint32_t payload_len, permute_handle_t *out)
{
    permute_process_t *sp = permute_process_get(k, sender);
    if (sp == NULL)
        return -ESRCH;

    if (payload_len > 0 && payload == NULL)
        return -EINVAL;

    uint32_t slot, gen;
    int rc = permute_table_alloc(&k->messages, &slot, &gen);
    if (rc != 0)
        return rc;

    permute_message_t *m = &k->message_pool[slot];
    memset(m, 0, sizeof *m);

    if (payload_len > 0) {
        m->payload = permute_blob_alloc(&k->blobs, payload_len);
        if (m->payload == NULL) {
            permute_table_free(&k->messages, slot);
            return -ENOMEM;
        }
        // Copy at send time. The SUT pointer is not retained (FR-3.7).
        memcpy(m->payload, payload, payload_len);
        m->payload_len = payload_len;
    }

    m->src_pid = sp->pid;
    m->src_tid = sender_tid;
    m->dst_pid = dst_pid;
    m->send_prio = send_prio;
    m->sequence = k->sequence++;
    m->rcvid = permute_rcvid_make(slot, gen);

    *out = PERMUTE_HANDLE_MAKE(slot, gen);
    return 0;
}

int permute_message_destroy(permute_kernel_t *k, permute_handle_t h)
{
    permute_message_t *m = permute_message_get(k, h);
    if (m == NULL)
        return -ESRCH;

    permute_blob_free(&k->blobs, m->payload);
    permute_blob_free(&k->blobs, m->reply);
    m->payload = NULL;
    m->reply = NULL;

    permute_table_free(&k->messages, PERMUTE_HANDLE_SLOT(h));
    return 0;
}

int permute_message_lookup_rcvid(permute_kernel_t *k, int32_t rcvid,
                                 permute_handle_t *out)
{
    if (!permute_rcvid_is_valid(rcvid))
        return -EINVAL;

    uint32_t slot = permute_rcvid_slot(rcvid);
    uint32_t gen = permute_rcvid_gen(rcvid);

    if (slot >= k->messages.capacity || !k->messages.in_use[slot])
        return -ESRCH;

    // The table holds 32 bits of generation; the rcvid carries 15. Compare the
    // low bits, which is what the encoding can express.
    if ((k->messages.generation[slot] & PERMUTE_RCVID_GEN_MASK) != gen)
        return -ESRCH;

    *out = PERMUTE_HANDLE_MAKE(slot, k->messages.generation[slot]);
    return 0;
}

// ---------------------------------------------------------------------------
// Pulses

permute_pulse_t *permute_pulse_get(permute_kernel_t *k, permute_handle_t h)
{
    if (!permute_table_valid(&k->pulses, h))
        return NULL;
    return &k->pulse_pool[PERMUTE_HANDLE_SLOT(h)];
}

int permute_pulse_create(permute_kernel_t *k, uint32_t src_pid,
                         uint32_t src_tid, int32_t code, uint64_t value,
                         int32_t priority, permute_handle_t *out)
{
    uint32_t slot, gen;
    int rc = permute_table_alloc(&k->pulses, &slot, &gen);
    if (rc != 0)
        return rc;

    permute_pulse_t *p = &k->pulse_pool[slot];
    memset(p, 0, sizeof *p);
    p->code = code;
    p->value = value; // opaque; never dereferenced
    p->priority = priority;
    p->scoid = 0;
    p->src_pid = src_pid; // 0 means the kernel
    p->src_tid = src_tid;
    p->sequence = k->sequence++;

    *out = PERMUTE_HANDLE_MAKE(slot, gen);
    return 0;
}

int permute_pulse_destroy(permute_kernel_t *k, permute_handle_t h)
{
    if (permute_pulse_get(k, h) == NULL)
        return -ESRCH;
    permute_table_free(&k->pulses, PERMUTE_HANDLE_SLOT(h));
    return 0;
}

// ---------------------------------------------------------------------------
// Queue

void permute_queue_init(permute_queue_t *q)
{
    memset(q, 0, sizeof *q);
}

int permute_queue_insert(permute_queue_t *q, permute_handle_t h,
                         int32_t priority, uint64_t sequence, uint8_t kind)
{
    if (q->length >= PERMUTE_QUEUE_CAP)
        return -EAGAIN;

    // Descending priority, ascending insertion sequence within a priority.
    uint32_t at = q->length;
    for (uint32_t i = 0; i < q->length; i++) {
        const permute_queue_entry_t *e = &q->entries[i];
        if (priority > e->priority ||
            (priority == e->priority && sequence < e->sequence)) {
            at = i;
            break;
        }
    }

    if (at < q->length) {
        memmove(&q->entries[at + 1], &q->entries[at],
                (q->length - at) * sizeof q->entries[0]);
    }

    q->entries[at].handle = h;
    q->entries[at].priority = priority;
    q->entries[at].sequence = sequence;
    q->entries[at].kind = kind;
    q->length++;
    return 0;
}

int permute_queue_pop(permute_queue_t *q, permute_queue_entry_t *out)
{
    if (q->length == 0)
        return -EAGAIN;

    *out = q->entries[0];
    memmove(&q->entries[0], &q->entries[1],
            (q->length - 1) * sizeof q->entries[0]);
    q->length--;
    return 0;
}

int permute_queue_pop_matching(permute_queue_t *q, uint8_t kind_mask,
                               permute_queue_entry_t *out)
{
    for (uint32_t i = 0; i < q->length; i++) {
        if ((q->entries[i].kind & kind_mask) == 0)
            continue;

        *out = q->entries[i];
        memmove(&q->entries[i], &q->entries[i + 1],
                (q->length - i - 1) * sizeof q->entries[0]);
        q->length--;
        return 0;
    }
    return -EAGAIN;
}

int permute_queue_remove(permute_queue_t *q, permute_handle_t h)
{
    for (uint32_t i = 0; i < q->length; i++) {
        if (q->entries[i].handle != h)
            continue;
        memmove(&q->entries[i], &q->entries[i + 1],
                (q->length - i - 1) * sizeof q->entries[0]);
        q->length--;
        return 0;
    }
    return -ESRCH;
}

uint32_t permute_queue_length(const permute_queue_t *q)
{
    return q->length;
}
