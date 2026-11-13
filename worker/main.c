// SPDX-License-Identifier: Apache-2.0
//
// permute-worker: run a seed range with process-level crash isolation.
//
// Each seed is a child process (`permute-run`). A child that dies by signal
// becomes a CRASH finding and the loop continues; the worker never executes
// SUT code itself. A child that overruns the wall budget is killed and
// recorded as BUDGET, which is an outcome rather than a failure (FR-5.6).
//
// Batches are claimed through lease files (see worker/lease.c), so several
// workers can share one output directory. The report is appended per seed and
// rewritten as a summary at the end, so a killed worker still leaves usable
// evidence.

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "worker/lease.h"

typedef enum {
    OUTCOME_CLEAN = 0,
    OUTCOME_CRASH,
    OUTCOME_BUDGET,
    OUTCOME_VIOLATION,
    OUTCOME_INTERNAL
} outcome_t;

static const char *const g_outcome_names[] = {
    "CLEAN", "CRASH", "BUDGET", "VIOLATION", "INTERNAL",
};

typedef struct {
    uint64_t first;
    uint64_t last;
    const char *out;
    const char *runner;
    const char *report;
    uint32_t batch;
    uint32_t wall_ms;
    uint32_t shard_index;
    uint32_t shard_count;
    int steal;
    int quiet;
} options_t;

typedef struct {
    uint64_t seed;
    outcome_t outcome;
    int signal;
} finding_t;

static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_child = -1;

// A worker asked to stop must take its child with it. Otherwise the runner
// outlives the run, and a spinning SUT keeps burning a core after SIGINT.
static void on_stop(int signo)
{
    (void)signo;
    g_stop = 1;
    if (g_child > 0)
        kill((pid_t)g_child, SIGKILL);
}

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static int make_dir(const char *path)
{
    if (mkdir(path, 0755) == 0)
        return 0;
    return errno == EEXIST ? 0 : -1;
}

static int seed_in_shard(const options_t *o, uint64_t seed)
{
    return o->shard_count == 0 || (seed % o->shard_count) == o->shard_index;
}

static void append_jsonl(const options_t *o, uint64_t seed, outcome_t outcome,
                         int signo)
{
    char path[PERMUTE_LEASE_PATH_MAX];
    snprintf(path, sizeof path, "%s/report.jsonl", o->out);

    int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd < 0)
        return;

    char line[256];
    int n = snprintf(line, sizeof line,
                     "{\"seed\":%llu,\"outcome\":\"%s\",\"signal\":%d}\n",
                     (unsigned long long)seed, g_outcome_names[outcome],
                     signo);
    if (write(fd, line, (size_t)n) != n)
        g_stop = 1;
    close(fd);
}

static outcome_t run_seed(const options_t *o, uint64_t seed, int *signal_out)
{
    char seed_text[32];
    char trace[PERMUTE_LEASE_PATH_MAX];
    char log[PERMUTE_LEASE_PATH_MAX];

    snprintf(seed_text, sizeof seed_text, "%llu", (unsigned long long)seed);
    snprintf(trace, sizeof trace, "%s/traces/seed-%llu.pmt", o->out,
             (unsigned long long)seed);
    snprintf(log, sizeof log, "%s/logs/seed-%llu.log", o->out,
             (unsigned long long)seed);

    pid_t pid = fork();
    if (pid < 0)
        return OUTCOME_INTERNAL;

    if (pid == 0) {
        int fd = open(log, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        char *args[] = { (char *)o->runner, seed_text, trace, NULL };
        execv(o->runner, args);
        _exit(127);
    }

    g_child = (sig_atomic_t)pid;
    uint64_t started = monotonic_ms();

    for (;;) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);

        if (r == pid) {
            g_child = -1;
            if (WIFSIGNALED(status)) {
                *signal_out = WTERMSIG(status);
                return OUTCOME_CRASH;
            }
            if (WIFEXITED(status)) {
                int code = WEXITSTATUS(status);
                if (code == 0)
                    return OUTCOME_CLEAN;
                if (code == 1)
                    return OUTCOME_VIOLATION;
                return OUTCOME_INTERNAL;
            }
            return OUTCOME_INTERNAL;
        }

        if (o->wall_ms > 0 && monotonic_ms() - started >= o->wall_ms) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            g_child = -1;
            return OUTCOME_BUDGET;
        }

        struct timespec nap = { 0, 1000000 }; // 1 ms
        nanosleep(&nap, NULL);
    }
}

static int parse_seeds(const char *text, uint64_t *first, uint64_t *last)
{
    unsigned long long a = 0, b = 0;
    if (sscanf(text, "%llu-%llu", &a, &b) == 2) {
        if (b < a)
            return -1;
        *first = a;
        *last = b;
        return 0;
    }
    if (sscanf(text, "%llu", &a) == 1) {
        *first = a;
        *last = a;
        return 0;
    }
    return -1;
}

static int parse_shard(const char *text, uint32_t *index, uint32_t *count)
{
    unsigned i = 0, n = 0;
    if (sscanf(text, "%u/%u", &i, &n) != 2 || n == 0 || i >= n)
        return -1;
    *index = i;
    *count = n;
    return 0;
}

int main(int argc, char **argv)
{
    options_t o;
    memset(&o, 0, sizeof o);
    o.batch = 64;
    o.wall_ms = 2000;
    o.report = NULL;

    int have_seeds = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--seeds") == 0 && i + 1 < argc) {
            if (parse_seeds(argv[++i], &o.first, &o.last) != 0) {
                fprintf(stderr, "bad --seeds\n");
                return 2;
            }
            have_seeds = 1;
        } else if (strcmp(a, "--out") == 0 && i + 1 < argc) {
            o.out = argv[++i];
        } else if (strcmp(a, "--runner") == 0 && i + 1 < argc) {
            o.runner = argv[++i];
        } else if (strcmp(a, "--report") == 0 && i + 1 < argc) {
            o.report = argv[++i];
        } else if (strcmp(a, "--batch") == 0 && i + 1 < argc) {
            o.batch = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(a, "--wall-ms") == 0 && i + 1 < argc) {
            o.wall_ms = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(a, "--shard") == 0 && i + 1 < argc) {
            if (parse_shard(argv[++i], &o.shard_index, &o.shard_count) != 0) {
                fprintf(stderr, "bad --shard\n");
                return 2;
            }
        } else if (strcmp(a, "--steal") == 0) {
            o.steal = 1;
        } else if (strcmp(a, "--quiet") == 0) {
            o.quiet = 1;
        } else {
            fprintf(stderr, "unknown option %s\n", a);
            return 2;
        }
    }

    if (!have_seeds || o.out == NULL || o.runner == NULL || o.batch == 0) {
        fprintf(stderr,
                "usage: %s --seeds A-B --out DIR --runner PATH "
                "[--batch N] [--wall-ms N] [--shard i/n] [--steal] "
                "[--report PATH] [--quiet]\n",
                argv[0]);
        return 2;
    }

    // A range must be finite and small enough to hold one finding per seed.
    uint64_t total = o.last - o.first + 1;
    if (o.last == UINT64_MAX || total == 0 || total > 10000000ull) {
        fprintf(stderr, "seed range is not supported\n");
        return 2;
    }

    char traces[PERMUTE_LEASE_PATH_MAX];
    char logs[PERMUTE_LEASE_PATH_MAX];
    char leases[PERMUTE_LEASE_PATH_MAX];
    snprintf(traces, sizeof traces, "%s/traces", o.out);
    snprintf(logs, sizeof logs, "%s/logs", o.out);
    snprintf(leases, sizeof leases, "%s/leases", o.out);

    if (make_dir(o.out) != 0 || make_dir(traces) != 0 ||
        make_dir(logs) != 0 || make_dir(leases) != 0) {
        fprintf(stderr, "cannot create output directories under %s: %s\n",
                o.out, strerror(errno));
        return 7;
    }

    char default_report[PERMUTE_LEASE_PATH_MAX];
    if (o.report == NULL) {
        snprintf(default_report, sizeof default_report, "%s/report.json",
                 o.out);
        o.report = default_report;
    }

    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);

    finding_t *findings = calloc((size_t)total, sizeof *findings);
    if (findings == NULL)
        return 7;

    uint64_t ran = 0;
    uint64_t counts[5] = { 0, 0, 0, 0, 0 };
    uint64_t batch_first = 0;
    uint64_t next = o.first;

    while (next <= o.last && !g_stop) {
        if (!seed_in_shard(&o, next)) {
            next++;
            continue;
        }

        // Collect this batch: a contiguous span, filtered by the shard.
        batch_first = next;
        uint64_t batch_last = next;
        uint32_t in_batch = 1;
        uint64_t scan = next + 1;
        while (scan <= o.last && in_batch < o.batch) {
            if (seed_in_shard(&o, scan)) {
                batch_last = scan;
                in_batch++;
            }
            scan++;
        }

        permute_lease_t lease;
        int claimed = permute_lease_claim(o.out, (uint32_t)batch_first,
                                          (uint32_t)batch_last, o.steal,
                                          &lease);
        if (claimed != 0) {
            if (!o.quiet) {
                fprintf(stderr, "batch %llu-%llu busy, skipping\n",
                        (unsigned long long)batch_first,
                        (unsigned long long)batch_last);
            }
            next = scan;
            continue;
        }

        for (uint64_t seed = batch_first; seed <= batch_last && !g_stop;
             seed++) {
            if (!seed_in_shard(&o, seed))
                continue;

            int signo = 0;
            outcome_t outcome = run_seed(&o, seed, &signo);
            if (g_stop)
                break; // interrupted: do not count a killed child as a finding
            counts[outcome]++;
            findings[ran].seed = seed;
            findings[ran].outcome = outcome;
            findings[ran].signal = signo;
            ran++;

            append_jsonl(&o, seed, outcome, signo);

            if (!o.quiet) {
                if (outcome == OUTCOME_CRASH)
                    printf("seed %llu: CRASH signal=%d\n",
                           (unsigned long long)seed, signo);
                else
                    printf("seed %llu: %s\n", (unsigned long long)seed,
                           g_outcome_names[outcome]);
            }
        }

        permute_lease_release(&lease);
        next = scan;
    }

    int exit_code = 0;
    if (counts[OUTCOME_CRASH] > 0 || counts[OUTCOME_VIOLATION] > 0)
        exit_code = 1;
    else if (counts[OUTCOME_INTERNAL] > 0)
        exit_code = 7;
    if (g_stop)
        exit_code = 130;

    FILE *report = fopen(o.report, "w");
    if (report == NULL) {
        free(findings);
        return 7;
    }

    fprintf(report,
            "{\"seeds\":%llu,\"clean\":%llu,\"crash\":%llu,\"budget\":%llu,"
            "\"violation\":%llu,\"internal\":%llu,\"exit_code\":%d,"
            "\"findings\":[",
            (unsigned long long)ran, (unsigned long long)counts[OUTCOME_CLEAN],
            (unsigned long long)counts[OUTCOME_CRASH],
            (unsigned long long)counts[OUTCOME_BUDGET],
            (unsigned long long)counts[OUTCOME_VIOLATION],
            (unsigned long long)counts[OUTCOME_INTERNAL], exit_code);

    int printed = 0;
    for (uint64_t i = 0; i < ran; i++) {
        if (findings[i].outcome == OUTCOME_CLEAN ||
            findings[i].outcome == OUTCOME_BUDGET)
            continue;
        fprintf(report, "%s{\"seed\":%llu,\"outcome\":\"%s\",\"signal\":%d}",
                printed++ ? "," : "", (unsigned long long)findings[i].seed,
                g_outcome_names[findings[i].outcome], findings[i].signal);
    }
    fprintf(report, "]}\n");
    fclose(report);

    if (!o.quiet) {
        printf("summary: %llu seeds, %llu clean, %llu crash, %llu budget, "
               "%llu violation, %llu internal\n",
               (unsigned long long)ran,
               (unsigned long long)counts[OUTCOME_CLEAN],
               (unsigned long long)counts[OUTCOME_CRASH],
               (unsigned long long)counts[OUTCOME_BUDGET],
               (unsigned long long)counts[OUTCOME_VIOLATION],
               (unsigned long long)counts[OUTCOME_INTERNAL]);
    }

    free(findings);
    return exit_code;
}
