#define _GNU_SOURCE
#include "locality_policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL at %s:%d: %s\n", __FILE__, __LINE__, #x); \
    return EXIT_FAILURE; } } while (0)

static void group(cpu_set_t *array, int cpu, const int *members, size_t count)
{
    CPU_ZERO(&array[cpu]);
    for (size_t i = 0; i < count; ++i) CPU_SET(members[i], &array[cpu]);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--discover") == 0) {
        LocalityTopology discovered;
        LocalityStatus status = locality_topology_discover(&discovered, NULL);
        if (status == LOCALITY_STATUS_ERROR) {
            fprintf(stderr, "FAIL: no usable CPU cache topology discovered from sysfs\n");
            return EXIT_FAILURE;
        }
        printf("PASS: sysfs topology discovery status=%s, CPUs=%zu\n",
               status == LOCALITY_STATUS_OK ? "complete" : "partial",
               discovered.discovered_cpu_count);
        cpu_set_t allowed = discovered.present_cpus;
        if (CPU_ISSET(0, &discovered.present_cpus)) {
            for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
                if (cpu != 0 && CPU_ISSET(cpu, &discovered.present_cpus)) {
                    LocalityScore score = locality_score_destination(
                        &discovered, 0, cpu, &allowed);
                    printf("CPU 0 -> CPU %d: locality score %d\n", cpu, (int)score);
                }
            }
        }
        return EXIT_SUCCESS;
    }

    LocalityTopology t;
    locality_topology_init(&t);
    const int p01[] = {0, 1}, p23[] = {2, 3}, all[] = {0, 1, 2, 3};
    for (int cpu = 0; cpu < 4; ++cpu) CPU_SET(cpu, &t.present_cpus);
    t.discovered_cpu_count = 4;
    for (int cpu = 0; cpu < 4; ++cpu) {
        group(t.l1_shared, cpu, cpu < 2 ? p01 : p23, 2);
        group(t.l2_shared, cpu, cpu < 2 ? p01 : p23, 2);
        group(t.l3_shared, cpu, all, 4);
    }

    cpu_set_t allowed;
    CPU_ZERO(&allowed); CPU_SET(1, &allowed); CPU_SET(2, &allowed); CPU_SET(3, &allowed);
    CHECK(locality_score_destination(&t, 0, 1, &allowed) == LOCALITY_SCORE_SAME_L1);
    CHECK(locality_score_destination(&t, 0, 2, &allowed) == LOCALITY_SCORE_SAME_L3);
    CHECK(locality_score_destination(&t, 0, 3, &allowed) == LOCALITY_SCORE_SAME_L3);
    CPU_CLR(2, &allowed);
    CHECK(locality_score_destination(&t, 0, 2, &allowed) == LOCALITY_SCORE_ERROR);
    CHECK(locality_score_destination(&t, 0, 0, &allowed) == LOCALITY_SCORE_ERROR);
    CHECK(locality_score_destination(&t, -1, 1, &allowed) == LOCALITY_SCORE_ERROR);

    locality_topology_init(&t);
    CPU_SET(0, &t.present_cpus); CPU_SET(1, &t.present_cpus); CPU_SET(1, &allowed);
    CHECK(locality_score_destination(&t, 0, 1, &allowed) == LOCALITY_SCORE_UNKNOWN);
    puts("PASS: cache-locality scoring, allowed-mask enforcement, invalid inputs, and unknown-topology fallback");
    return EXIT_SUCCESS;
}
