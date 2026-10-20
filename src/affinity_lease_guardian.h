#ifndef AFFINITY_LEASE_GUARDIAN_H
#define AFFINITY_LEASE_GUARDIAN_H

#include <sched.h>
#include <time.h>
#include <sys/types.h>

#define AFFINITY_LEASE_CAPACITY 6

typedef struct {
    int active;
    pid_t pid;
    pid_t tid;
    unsigned long long start_ticks;
    int destination_cpu;
    cpu_set_t original_mask;
    struct timespec expires_at;
    int guardian_registered;
} AffinityLease;

typedef struct {
    int write_fd;
    int ack_fd;
    pid_t pid;
} AffinityLeaseGuardian;

int affinity_lease_guardian_start(AffinityLeaseGuardian *guardian);
int affinity_lease_guardian_register(AffinityLeaseGuardian *guardian,
                                     const AffinityLease *lease);
int affinity_lease_guardian_cancel(AffinityLeaseGuardian *guardian,
                                   const AffinityLease *lease);
void affinity_lease_guardian_stop(AffinityLeaseGuardian *guardian);

#endif
