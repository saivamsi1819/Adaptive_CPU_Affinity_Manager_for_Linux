#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "affinity_lease_guardian.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int read_start_ticks(pid_t pid, unsigned long long *start_ticks)
{
    char path[128];
    if (snprintf(path, sizeof(path), "/proc/%ld/task/%ld/stat",
                 (long)pid, (long)pid) >= (int)sizeof(path))
        return -1;
    FILE *file = fopen(path, "r");
    if (file == NULL)
        return -1;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length = getline(&line, &capacity, file);
    fclose(file);
    if (length < 0) {
        free(line);
        return -1;
    }
    char *right_paren = strrchr(line, ')');
    if (right_paren == NULL) {
        free(line);
        return -1;
    }
    char *save = NULL;
    char *token = strtok_r(right_paren + 1, " \t\n", &save);
    for (int field = 3; token != NULL && field < 22; ++field)
        token = strtok_r(NULL, " \t\n", &save);
    if (token == NULL) {
        free(line);
        return -1;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(token, &end, 10);
    int valid = errno == 0 && end != token && *end == '\0';
    free(line);
    if (!valid)
        return -1;
    *start_ticks = parsed;
    return 0;
}

static int masks_equal(const cpu_set_t *left, const cpu_set_t *right)
{
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, left) != CPU_ISSET(cpu, right))
            return 0;
    }
    return 1;
}

static int first_cpu(const cpu_set_t *mask)
{
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, mask))
            return cpu;
    }
    return -1;
}

int main(void)
{
    cpu_set_t original_mask;
    CPU_ZERO(&original_mask);
    if (sched_getaffinity(0, sizeof(original_mask), &original_mask) != 0) {
        perror("sched_getaffinity self");
        return 1;
    }
    if (CPU_COUNT(&original_mask) < 2) {
        puts("SKIP: guardian crash test needs at least two allowed CPUs");
        return 0;
    }

    int hold_pipe[2];
    if (pipe(hold_pipe) != 0) {
        perror("pipe worker hold");
        return 1;
    }
    pid_t worker = fork();
    if (worker < 0) {
        perror("fork worker");
        close(hold_pipe[0]);
        close(hold_pipe[1]);
        return 1;
    }
    if (worker == 0) {
        close(hold_pipe[1]);
        char byte;
        while (read(hold_pipe[0], &byte, 1) > 0) {
        }
        _exit(0);
    }
    close(hold_pipe[0]);

    cpu_set_t worker_original;
    CPU_ZERO(&worker_original);
    if (sched_getaffinity(worker, sizeof(worker_original),
                          &worker_original) != 0) {
        perror("sched_getaffinity worker");
        close(hold_pipe[1]);
        (void)waitpid(worker, NULL, 0);
        return 1;
    }
    int destination_cpu = first_cpu(&worker_original);
    unsigned long long start_ticks = 0;
    if (destination_cpu < 0 || read_start_ticks(worker, &start_ticks) != 0) {
        fprintf(stderr, "Could not identify test worker\n");
        close(hold_pipe[1]);
        (void)waitpid(worker, NULL, 0);
        return 1;
    }

    pid_t simulated_manager = fork();
    if (simulated_manager < 0) {
        perror("fork simulated manager");
        close(hold_pipe[1]);
        (void)waitpid(worker, NULL, 0);
        return 1;
    }
    if (simulated_manager == 0) {
        close(hold_pipe[1]);
        AffinityLeaseGuardian guardian;
        if (affinity_lease_guardian_start(&guardian) != 0)
            _exit(2);

        AffinityLease lease = {0};
        lease.active = 1;
        lease.pid = worker;
        lease.tid = worker;
        lease.start_ticks = start_ticks;
        lease.destination_cpu = destination_cpu;
        lease.original_mask = worker_original;
        if (clock_gettime(CLOCK_MONOTONIC, &lease.expires_at) != 0)
            _exit(3);
        lease.expires_at.tv_sec += 5;

        if (affinity_lease_guardian_register(&guardian, &lease) != 0)
            _exit(4);
        cpu_set_t destination_mask;
        CPU_ZERO(&destination_mask);
        CPU_SET(destination_cpu, &destination_mask);
        if (sched_setaffinity(worker, sizeof(destination_mask),
                              &destination_mask) != 0)
            _exit(5);

        /* Simulate a manager crash: exit without stopping the guardian. */
        _exit(0);
    }

    int manager_status = 0;
    while (waitpid(simulated_manager, &manager_status, 0) < 0) {
        if (errno == EINTR)
            continue;
        perror("waitpid simulated manager");
        close(hold_pipe[1]);
        (void)waitpid(worker, NULL, 0);
        return 1;
    }

    int restored = 0;
    for (int attempt = 0; attempt < 200; ++attempt) {
        cpu_set_t current_mask;
        CPU_ZERO(&current_mask);
        if (sched_getaffinity(worker, sizeof(current_mask), &current_mask) == 0 &&
            masks_equal(&current_mask, &worker_original)) {
            restored = 1;
            break;
        }
        struct timespec delay = {0, 10000000L};
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
        }
    }

    close(hold_pipe[1]);
    (void)waitpid(worker, NULL, 0);
    if (!WIFEXITED(manager_status) || WEXITSTATUS(manager_status) != 0) {
        fprintf(stderr, "FAIL: simulated manager exited with status %d\n",
                manager_status);
        return 1;
    }
    if (!restored) {
        fprintf(stderr,
                "FAIL: guardian did not restore the test worker's original CPU mask\n");
        return 1;
    }
    puts("PASS: independent guardian restored a test worker after manager exit");
    return 0;
}
