// SPDX-License-Identifier: Apache-2.0
//
// Cooperative executor: one host thread per simulated thread, a baton, and
// the ready queue as the only source of scheduling decisions.
//
// The baton protocol is two semaphores per thread: the scheduler posts `wake`
// and waits on `yielded`; a guest thread that blocks or yields posts `yielded`
// and waits on `wake`. Exactly one guest thread is between those two points at
// any time, which is the single-CPU invariant in executable form.

#define _GNU_SOURCE

#include "runtime/runtime.h"

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/objects.h"
#include "kernel/sched.h"
#include "kernel/timeout.h"

typedef struct sim_thread {
    permute_handle_t handle;
    pthread_t host;
    sem_t wake;
    sem_t yielded;

    void *(*start)(void *);
    void *arg;
    void *retval;
    int is_main;
    int done;
    int cancelled;

    struct executor *ex;
} sim_thread;

typedef struct executor {
    permute_kernel_t *k;
    permute_handle_t process;
    permute_guest_main_fn main_fn;
    int argc;
    char **argv;
    int main_rc;

    sim_thread *threads[PERMUTE_MAX_THREADS];
    int n_threads;
    int live;
} executor_t;

static executor_t *g_executor;

__thread permute_handle_t permute_runtime_current;
static __thread sim_thread *g_self;

static sim_thread *find_thread(executor_t *ex, permute_handle_t handle)
{
    for (int i = 0; i < ex->n_threads; i++) {
        if (ex->threads[i]->handle == handle)
            return ex->threads[i];
    }
    return NULL;
}

static void executor_finish(sim_thread *st, void *retval)
{
    if (st->done)
        return;

    st->done = 1;
    st->retval = retval;

    executor_t *ex = st->ex;
    permute_kernel_t *k = ex->k;

    if (st->is_main)
        ex->main_rc = (int)(intptr_t)retval;

    // Threads waiting in JOIN on this one become runnable.
    for (uint32_t i = 0; i < k->threads.capacity; i++) {
        if (!k->threads.in_use[i])
            continue;
        permute_thread_t *t = &k->thread_pool[i];
        if (t->state == PERMUTE_THREAD_JOIN && t->wobj == st->handle)
            permute_thread_wake(k, t, 0, 0, 1);
    }

    permute_thread_destroy(k, st->handle);
    ex->live--;
    sem_post(&st->yielded);
}

static void *host_thread(void *arg)
{
    sim_thread *st = arg;
    g_self = st;
    permute_runtime_current = st->handle;

    sem_wait(&st->wake);

    void *ret;
    if (st->is_main)
        ret = (void *)(intptr_t)st->ex->main_fn(st->ex->argc, st->ex->argv);
    else
        ret = st->start(st->arg);

    executor_finish(st, ret);
    return NULL;
}

static sim_thread *executor_add(executor_t *ex, permute_handle_t handle,
                                void *(*start)(void *), void *arg, int is_main)
{
    if (ex->n_threads >= PERMUTE_MAX_THREADS)
        return NULL;

    sim_thread *st = calloc(1, sizeof *st);
    if (st == NULL)
        return NULL;

    st->handle = handle;
    st->start = start;
    st->arg = arg;
    st->is_main = is_main;
    st->ex = ex;

    if (sem_init(&st->wake, 0, 0) != 0 || sem_init(&st->yielded, 0, 0) != 0) {
        free(st);
        return NULL;
    }

    if (pthread_create(&st->host, NULL, host_thread, st) != 0) {
        sem_destroy(&st->wake);
        sem_destroy(&st->yielded);
        free(st);
        return NULL;
    }

    ex->threads[ex->n_threads++] = st;
    ex->live++;
    return st;
}

void permute_executor_park(void)
{
    if (g_self == NULL)
        return; // not running under the executor

    if (g_self->cancelled) {
        executor_finish(g_self, NULL);
        pthread_exit(NULL);
    }

    sem_post(&g_self->yielded);
    sem_wait(&g_self->wake);

    if (g_self->cancelled) {
        executor_finish(g_self, NULL);
        pthread_exit(NULL);
    }
}

int permute_executor_thread_create(uint64_t *token, const pthread_attr_t *attr,
                                   void *(*start)(void *), void *arg)
{
    (void)attr;

    executor_t *ex = g_executor;
    if (ex == NULL)
        return EAGAIN;

    permute_thread_t *parent =
        permute_thread_get(ex->k, permute_runtime_current);
    if (parent == NULL)
        return EINVAL;

    permute_handle_t handle;
    int rc = permute_thread_create(ex->k, parent->process, parent->base_prio,
                                   PERMUTE_SCHED_FIFO, &handle);
    if (rc != 0)
        return EAGAIN;

    if (executor_add(ex, handle, start, arg, 0) == NULL) {
        permute_thread_destroy(ex->k, handle);
        return EAGAIN;
    }

    *token = (uint64_t)handle;
    return 0;
}

int permute_executor_thread_join(uint64_t token, void **retval)
{
    executor_t *ex = g_executor;
    if (ex == NULL)
        return EINVAL;

    sim_thread *st = find_thread(ex, (permute_handle_t)token);
    if (st == NULL)
        return ESRCH;

    if (!st->done) {
        permute_thread_t *self =
            permute_thread_get(ex->k, permute_runtime_current);
        if (self == NULL)
            return EINVAL;
        permute_thread_block(ex->k, self, PERMUTE_THREAD_JOIN, st->handle);
        permute_executor_park();
    }

    if (retval != NULL)
        *retval = st->retval;
    return 0;
}

void permute_executor_thread_exit(void *retval)
{
    if (g_self != NULL)
        executor_finish(g_self, retval);

    pthread_exit(NULL); // does not return
}

int permute_executor_thread_cancel(uint64_t token)
{
    executor_t *ex = g_executor;
    if (ex == NULL)
        return EINVAL;

    sim_thread *st = find_thread(ex, (permute_handle_t)token);
    if (st == NULL)
        return ESRCH;
    if (st->done)
        return 0;

    // The target leaves at its next park, which is the next shim boundary.
    st->cancelled = 1;
    sem_post(&st->wake);
    return 0;
}

int permute_executor_run(permute_executor_t *ex, permute_guest_main_fn main_fn,
                         int *guest_rc, permute_exec_outcome_t *outcome)
{
    executor_t runner;
    memset(&runner, 0, sizeof runner);
    runner.k = ex->k;
    runner.process = ex->process;
    runner.main_fn = main_fn;
    runner.argc = ex->argc;
    runner.argv = ex->argv;

    g_executor = &runner;

    permute_handle_t main_handle;
    int rc = permute_thread_create(ex->k, ex->process, 10, PERMUTE_SCHED_FIFO,
                                   &main_handle);
    if (rc != 0) {
        *outcome = PERMUTE_EXEC_INTERNAL;
        return -1;
    }

    if (executor_add(&runner, main_handle, NULL, NULL, 1) == NULL) {
        *outcome = PERMUTE_EXEC_INTERNAL;
        return -1;
    }

    while (runner.live > 0) {
        permute_handle_t next = permute_sched_pick(ex->k);

        if (next == PERMUTE_HANDLE_NONE) {
            permute_sim_outcome_t sim = permute_sim_advance(ex->k);
            if (sim == PERMUTE_SIM_DEADLOCK) {
                *outcome = PERMUTE_EXEC_DEADLOCK;
                *guest_rc = -1;
                g_executor = NULL;
                return 0;
            }
            continue;
        }

        sim_thread *st = find_thread(&runner, next);
        if (st == NULL) {
            // A kernel thread with no host thread: nothing can run it.
            permute_thread_destroy(ex->k, next);
            continue;
        }

        sem_post(&st->wake);
        sem_wait(&st->yielded);
    }

    *guest_rc = runner.main_rc;
    *outcome = runner.main_rc == 0 ? PERMUTE_EXEC_CLEAN
                                   : PERMUTE_EXEC_GUEST_FAILED;
    g_executor = NULL;
    return 0;
}
