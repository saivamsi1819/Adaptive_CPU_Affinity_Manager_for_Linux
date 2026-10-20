#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "affinity_lease_guardian.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum {
    LEASE_GUARDIAN_ADD = 1,
    LEASE_GUARDIAN_CANCEL = 2
};

typedef struct {
    int operation;
    AffinityLease lease;
} LeaseGuardianMessage;

static int read_task_start_ticks(pid_t pid, pid_t tid,
                                 unsigned long long *start_ticks)
{
    if (start_ticks == NULL) {
        errno = EINVAL;
        return -1;
    }

    char path[128];
    int path_length = snprintf(path, sizeof(path),
                               "/proc/%ld/task/%ld/stat",
                               (long)pid, (long)tid);
    if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL)
        return -1;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length = getline(&line, &capacity, file);
    int saved_errno = errno;
    fclose(file);
    if (length < 0) {
        free(line);
        errno = saved_errno ? saved_errno : EIO;
        return -1;
    }

    char *right_paren = strrchr(line, ')');
    if (right_paren == NULL || right_paren[1] == '\0') {
        free(line);
        errno = EPROTO;
        return -1;
    }
    char *save = NULL;
    char *token = strtok_r(right_paren + 1, " \t\n", &save);
    for (int field = 3; token != NULL && field < 22; ++field)
        token = strtok_r(NULL, " \t\n", &save);
    if (token == NULL) {
        free(line);
        errno = EPROTO;
        return -1;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(token, &end, 10);
    if (errno != 0 || end == token || *end != '\0') {
        free(line);
        errno = EPROTO;
        return -1;
    }
    *start_ticks = parsed;
    free(line);
    return 0;
}

static int task_is_owned_identity(const AffinityLease *lease)
{
    unsigned long long current_start_ticks = 0;
    if (read_task_start_ticks(lease->pid, lease->tid,
                              &current_start_ticks) != 0)
        return -1;
    if (current_start_ticks != lease->start_ticks) {
        errno = ESRCH;
        return 0;
    }

    char task_path[128];
    int length = snprintf(task_path, sizeof(task_path),
                          "/proc/%ld/task/%ld",
                          (long)lease->pid, (long)lease->tid);
    if (length < 0 || (size_t)length >= sizeof(task_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    struct stat task_info;
    if (stat(task_path, &task_info) != 0)
        return -1;
    if (task_info.st_uid != geteuid()) {
        errno = EPERM;
        return 0;
    }
    return 1;
}

static int affinity_masks_equal(const cpu_set_t *left, const cpu_set_t *right)
{
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, left) != CPU_ISSET(cpu, right))
            return 0;
    }
    return 1;
}

static void restore_lease(AffinityLease *lease, const char *reason)
{
    if (!lease->active)
        return;
    if (task_is_owned_identity(lease) != 1) {
        fprintf(stderr,
                "[lease-guardian] restore skipped for TID %ld: task exited, identity changed, or ownership changed (%s)\n",
                (long)lease->tid, reason);
        lease->active = 0;
        return;
    }

    cpu_set_t current_mask;
    cpu_set_t leased_mask;
    CPU_ZERO(&current_mask);
    CPU_ZERO(&leased_mask);
    CPU_SET(lease->destination_cpu, &leased_mask);
    if (sched_getaffinity(lease->tid, sizeof(current_mask),
                          &current_mask) != 0) {
        fprintf(stderr,
                "[lease-guardian] sched_getaffinity failed for TID %ld: %s\n",
                (long)lease->tid, strerror(errno));
        return;
    }
    if (!affinity_masks_equal(&current_mask, &leased_mask)) {
        /* The manager may have restored it already, or another actor changed it. */
        lease->active = 0;
        return;
    }
    if (sched_setaffinity(lease->tid, sizeof(lease->original_mask),
                          &lease->original_mask) != 0) {
        fprintf(stderr,
                "[lease-guardian] restoration failed for TID %ld: %s\n",
                (long)lease->tid, strerror(errno));
        return;
    }
    fprintf(stderr,
            "[lease-guardian] restored TID %ld original affinity (%s)\n",
            (long)lease->tid, reason);
    lease->active = 0;
}

static int send_message(int fd, int operation, const AffinityLease *lease)
{
    LeaseGuardianMessage message;
    memset(&message, 0, sizeof(message));
    message.operation = operation;
    if (lease != NULL)
        message.lease = *lease;
    ssize_t written;
    do {
        written = write(fd, &message, sizeof(message));
    } while (written < 0 && errno == EINTR);
    if (written != (ssize_t)sizeof(message)) {
        if (written >= 0)
            errno = EIO;
        return -1;
    }
    return 0;
}

static int read_message(int fd, LeaseGuardianMessage *message)
{
    size_t received = 0;
    while (received < sizeof(*message)) {
        ssize_t count = read(fd, (char *)message + received,
                             sizeof(*message) - received);
        if (count == 0)
            return received == 0 ? 0 : -1;
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        received += (size_t)count;
    }
    return 1;
}

static void guardian_loop(int read_fd, int ack_fd)
{
    AffinityLease leases[AFFINITY_LEASE_CAPACITY] = {0};
    for (;;) {
        struct pollfd descriptor = {.fd = read_fd, .events = POLLIN};
        int ready;
        do {
            ready = poll(&descriptor, 1, -1);
        } while (ready < 0 && errno == EINTR);
        if (ready < 0)
            break;

        LeaseGuardianMessage message;
        int read_status = read_message(read_fd, &message);
        if (read_status <= 0)
            break;
        if (message.operation == LEASE_GUARDIAN_ADD) {
            size_t slot = AFFINITY_LEASE_CAPACITY;
            for (size_t i = 0; i < AFFINITY_LEASE_CAPACITY; ++i) {
                if (leases[i].active &&
                    leases[i].pid == message.lease.pid &&
                    leases[i].tid == message.lease.tid &&
                    leases[i].start_ticks == message.lease.start_ticks) {
                    slot = i;
                    break;
                }
                if (!leases[i].active)
                    slot = i;
            }
            if (slot < AFFINITY_LEASE_CAPACITY)
                leases[slot] = message.lease;
            int ack = slot < AFFINITY_LEASE_CAPACITY ? 0 : ENOSPC;
            ssize_t sent;
            do {
                sent = write(ack_fd, &ack, sizeof(ack));
            } while (sent < 0 && errno == EINTR);
            if (sent != (ssize_t)sizeof(ack))
                break;
        } else if (message.operation == LEASE_GUARDIAN_CANCEL) {
            for (size_t i = 0; i < AFFINITY_LEASE_CAPACITY; ++i) {
                if (leases[i].active &&
                    leases[i].pid == message.lease.pid &&
                    leases[i].tid == message.lease.tid &&
                    leases[i].start_ticks == message.lease.start_ticks)
                    leases[i].active = 0;
            }
        }
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        for (size_t i = 0; i < AFFINITY_LEASE_CAPACITY; ++i)
            restore_lease(&leases[i], "controller exit");
        int active = 0;
        for (size_t i = 0; i < AFFINITY_LEASE_CAPACITY; ++i)
            active |= leases[i].active;
        if (!active)
            break;
        struct timespec delay = {0, 50000000L};
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
        }
    }
    close(read_fd);
    close(ack_fd);
}

int affinity_lease_guardian_start(AffinityLeaseGuardian *guardian)
{
    if (guardian == NULL) {
        errno = EINVAL;
        return -1;
    }
    guardian->write_fd = -1;
    guardian->ack_fd = -1;
    guardian->pid = -1;
    int requests[2];
    int acknowledgements[2];
    if (pipe(requests) != 0)
        return -1;
    if (pipe(acknowledgements) != 0) {
        int saved_errno = errno;
        close(requests[0]);
        close(requests[1]);
        errno = saved_errno;
        return -1;
    }
    pid_t child = fork();
    if (child < 0) {
        int saved_errno = errno;
        close(requests[0]);
        close(requests[1]);
        close(acknowledgements[0]);
        close(acknowledgements[1]);
        errno = saved_errno;
        return -1;
    }
    if (child == 0) {
        close(requests[1]);
        close(acknowledgements[0]);
        guardian_loop(requests[0], acknowledgements[1]);
        _exit(0);
    }
    close(requests[0]);
    close(acknowledgements[1]);
    guardian->write_fd = requests[1];
    guardian->ack_fd = acknowledgements[0];
    guardian->pid = child;
    return 0;
}

int affinity_lease_guardian_register(AffinityLeaseGuardian *guardian,
                                     const AffinityLease *lease)
{
    if (guardian == NULL || guardian->write_fd < 0 ||
        guardian->ack_fd < 0 || lease == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (send_message(guardian->write_fd, LEASE_GUARDIAN_ADD, lease) != 0)
        return -1;
    int ack = -1;
    size_t received = 0;
    while (received < sizeof(ack)) {
        ssize_t count = read(guardian->ack_fd,
                             (char *)&ack + received,
                             sizeof(ack) - received);
        if (count == 0) {
            errno = EPIPE;
            return -1;
        }
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        received += (size_t)count;
    }
    if (ack != 0) {
        errno = ack;
        return -1;
    }
    return 0;
}

int affinity_lease_guardian_cancel(AffinityLeaseGuardian *guardian,
                                   const AffinityLease *lease)
{
    if (guardian == NULL || guardian->write_fd < 0 || lease == NULL) {
        errno = EINVAL;
        return -1;
    }
    return send_message(guardian->write_fd, LEASE_GUARDIAN_CANCEL, lease);
}

void affinity_lease_guardian_stop(AffinityLeaseGuardian *guardian)
{
    if (guardian == NULL)
        return;
    if (guardian->write_fd >= 0) {
        close(guardian->write_fd);
        guardian->write_fd = -1;
    }
    if (guardian->ack_fd >= 0) {
        close(guardian->ack_fd);
        guardian->ack_fd = -1;
    }
    if (guardian->pid > 0) {
        int status;
        while (waitpid(guardian->pid, &status, 0) < 0 && errno == EINTR) {
        }
        guardian->pid = -1;
    }
}
