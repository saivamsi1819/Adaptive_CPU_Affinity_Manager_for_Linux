#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "locality_policy.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int valid_cpu(int cpu) { return cpu >= 0 && cpu < CPU_SETSIZE; }

void locality_topology_init(LocalityTopology *topology)
{
    if (!topology) return;
    CPU_ZERO(&topology->present_cpus);
    for (int i = 0; i < CPU_SETSIZE; ++i) {
        CPU_ZERO(&topology->l1_shared[i]);
        CPU_ZERO(&topology->l2_shared[i]);
        CPU_ZERO(&topology->l3_shared[i]);
    }
    topology->discovered_cpu_count = 0;
}

/* Linux CPU-list parser: examples "0-3,8,10-11". */
static int parse_cpu_list(const char *s, cpu_set_t *set)
{
    if (!s || !set) return -1;
    CPU_ZERO(set);
    const char *p = s;
    int any = 0;
    while (*p) {
        while (isspace((unsigned char)*p)) ++p;
        if (!*p) break;
        if (!isdigit((unsigned char)*p)) return -1;
        errno = 0;
        char *end = NULL;
        long first = strtol(p, &end, 10);
        if (errno || end == p || first < 0 || first >= CPU_SETSIZE) return -1;
        p = end;
        long last = first;
        if (*p == '-') {
            ++p;
            if (!isdigit((unsigned char)*p)) return -1;
            errno = 0;
            last = strtol(p, &end, 10);
            if (errno || end == p || last < first || last >= CPU_SETSIZE) return -1;
            p = end;
        }
        for (long cpu = first; cpu <= last; ++cpu) CPU_SET((int)cpu, set);
        any = 1;
        while (isspace((unsigned char)*p)) ++p;
        if (!*p) break;
        if (*p++ != ',' || !*p) return -1;
    }
    return any ? 0 : -1;
}

static int read_text(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int ok = fgets(buf, (int)size, f) != NULL;
    int close_ok = fclose(f) == 0;
    if (!ok || !close_ok) return -1;
    buf[strcspn(buf, "\r\n")] = '\0';
    return 0;
}

static int read_level(const char *path, int *level)
{
    char buf[64], *end = NULL;
    if (read_text(path, buf, sizeof(buf))) return -1;
    errno = 0;
    long v = strtol(buf, &end, 10);
    if (errno || end == buf || *end || v < 0 || v > INT_MAX) return -1;
    *level = (int)v;
    return 0;
}

static int discover_one(LocalityTopology *t, const char *root, int cpu)
{
    char path[PATH_MAX], cache[PATH_MAX], file[PATH_MAX], buf[4096];
    if (snprintf(path, sizeof(path), "%s/cpu%d", root, cpu) >= (int)sizeof(path)) return -1;
    FILE *probe = fopen(path, "r");
    if (!probe) return errno == ENOENT || errno == ENOTDIR ? 1 : -1;
    fclose(probe);

    int got_l1 = 0, got_l2 = 0, got_l3 = 0;
    for (int index = 0; index < 64; ++index) {
        if (snprintf(cache, sizeof(cache), "%s/cache/index%d", path, index) >= (int)sizeof(cache)) break;
        if (snprintf(file, sizeof(file), "%s/level", cache) >= (int)sizeof(file)) continue;
        int level;
        if (read_level(file, &level)) continue;
        if (snprintf(file, sizeof(file), "%s/type", cache) >= (int)sizeof(file) ||
            read_text(file, buf, sizeof(buf))) continue;
        char type[4096];
        (void)snprintf(type, sizeof(type), "%s", buf);
        if (snprintf(file, sizeof(file), "%s/shared_cpu_list", cache) >= (int)sizeof(file) ||
            read_text(file, buf, sizeof(buf))) continue;
        cpu_set_t shared;
        if (parse_cpu_list(buf, &shared) || !CPU_ISSET(cpu, &shared)) continue;

        if (level == 1 && strcmp(type, "Data") == 0 && !got_l1) {
            t->l1_shared[cpu] = shared; got_l1 = 1;
        } else if (level == 2 && strcmp(type, "Unified") == 0 && !got_l2) {
            t->l2_shared[cpu] = shared; got_l2 = 1;
        } else if (level == 3 && strcmp(type, "Unified") == 0 && !got_l3) {
            t->l3_shared[cpu] = shared; got_l3 = 1;
        }
    }
    if (!got_l1 && !got_l2 && !got_l3) return -1;
    CPU_SET(cpu, &t->present_cpus);
    ++t->discovered_cpu_count;
    return 0;
}

LocalityStatus locality_topology_discover(LocalityTopology *topology,
                                         const char *sysfs_cpu_root)
{
    if (!topology) return LOCALITY_STATUS_ERROR;
    locality_topology_init(topology);
    const char *root = sysfs_cpu_root ? sysfs_cpu_root : "/sys/devices/system/cpu";
    int found = 0, partial = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        int status = discover_one(topology, root, cpu);
        if (status == 0) ++found;
        else if (status < 0) partial = 1;
    }
    if (!found) return LOCALITY_STATUS_ERROR;
    return partial ? LOCALITY_STATUS_PARTIAL : LOCALITY_STATUS_OK;
}

static int same_domain(const cpu_set_t *a, const cpu_set_t *b, int src, int dst)
{
    return CPU_ISSET(src, a) && CPU_ISSET(dst, a) &&
           CPU_ISSET(src, b) && CPU_ISSET(dst, b);
}

LocalityScore locality_score_destination(const LocalityTopology *t,
                                         int source_cpu,
                                         int destination_cpu,
                                         const cpu_set_t *allowed_cpus)
{
    if (!t || !allowed_cpus || !valid_cpu(source_cpu) || !valid_cpu(destination_cpu) ||
        source_cpu == destination_cpu || !CPU_ISSET(destination_cpu, allowed_cpus))
        return LOCALITY_SCORE_ERROR;
    if (!CPU_ISSET(source_cpu, &t->present_cpus) ||
        !CPU_ISSET(destination_cpu, &t->present_cpus)) return LOCALITY_SCORE_UNKNOWN;

    if (same_domain(&t->l1_shared[source_cpu], &t->l1_shared[destination_cpu], source_cpu, destination_cpu))
        return LOCALITY_SCORE_SAME_L1;
    if (same_domain(&t->l2_shared[source_cpu], &t->l2_shared[destination_cpu], source_cpu, destination_cpu))
        return LOCALITY_SCORE_SAME_L2;
    if (same_domain(&t->l3_shared[source_cpu], &t->l3_shared[destination_cpu], source_cpu, destination_cpu))
        return LOCALITY_SCORE_SAME_L3;

    /* Missing cache information is unknown, not proof of remote locality. */
    if (CPU_COUNT(&t->l1_shared[source_cpu]) <= 1 &&
        CPU_COUNT(&t->l2_shared[source_cpu]) <= 1 &&
        CPU_COUNT(&t->l3_shared[source_cpu]) <= 1) return LOCALITY_SCORE_UNKNOWN;
    if (CPU_COUNT(&t->l1_shared[destination_cpu]) <= 1 &&
        CPU_COUNT(&t->l2_shared[destination_cpu]) <= 1 &&
        CPU_COUNT(&t->l3_shared[destination_cpu]) <= 1) return LOCALITY_SCORE_UNKNOWN;
    return LOCALITY_SCORE_DIFFERENT_DOMAIN;
}
