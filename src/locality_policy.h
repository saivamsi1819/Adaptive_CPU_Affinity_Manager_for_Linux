#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef LOCALITY_POLICY_H
#define LOCALITY_POLICY_H

#include <sched.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOCALITY_STATUS_ERROR = -1,
    LOCALITY_STATUS_OK = 0,
    LOCALITY_STATUS_PARTIAL = 1
} LocalityStatus;

typedef struct {
    cpu_set_t present_cpus;
    cpu_set_t l1_shared[CPU_SETSIZE];
    cpu_set_t l2_shared[CPU_SETSIZE];
    cpu_set_t l3_shared[CPU_SETSIZE];
    size_t discovered_cpu_count;
} LocalityTopology;

typedef enum {
    LOCALITY_SCORE_ERROR = -1,
    LOCALITY_SCORE_UNKNOWN = 0,
    LOCALITY_SCORE_SAME_L3 = 10,
    LOCALITY_SCORE_SAME_L2 = 20,
    LOCALITY_SCORE_SAME_L1 = 30,
    LOCALITY_SCORE_DIFFERENT_DOMAIN = -10
} LocalityScore;

void locality_topology_init(LocalityTopology *topology);

/* Pass NULL to use /sys/devices/system/cpu. */
LocalityStatus locality_topology_discover(
    LocalityTopology *topology,
    const char *sysfs_cpu_root);

/* Destination must be in allowed_cpus; affinity is never widened. */
LocalityScore locality_score_destination(
    const LocalityTopology *topology,
    int source_cpu,
    int destination_cpu,
    const cpu_set_t *allowed_cpus);

#ifdef __cplusplus
}
#endif

#endif /* LOCALITY_POLICY_H */
