#define _GNU_SOURCE
#include "candidate_selector.h"

#include <stdio.h>
#include <string.h>

static int expect(const char *label, int condition)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", label);
        return 0;
    }
    return 1;
}

int main(void)
{
    ThreadSample thread;
    cpu_set_t allowed;
    int ok = 1;

    memset(&thread, 0, sizeof(thread));
    thread.pid = 100;
    thread.tid = 101;
    thread.last_cpu = 2;
    thread.utilization_valid = 1;
    thread.utilization_percent = 42.0;
    snprintf(thread.name, sizeof(thread.name), "synthetic-worker");

    CPU_ZERO(&allowed);
    CPU_SET(2, &allowed);
    CPU_SET(3, &allowed);

    ok &= expect("eligible task with source and destination allowed",
                 candidate_thread_is_eligible(&thread, 2, 3, &allowed, 5.0));
    ok &= expect("reject task whose last CPU is not source",
                 !candidate_thread_is_eligible(&thread, 1, 3, &allowed, 5.0));
    cpu_set_t only_source;
    CPU_ZERO(&only_source);
    CPU_SET(2, &only_source);
    ok &= expect("reject task if destination is not allowed",
                 !candidate_thread_is_eligible(&thread, 2, 3, &only_source, 5.0));
    ok &= expect("reject task below utilization threshold", 
                 !candidate_thread_is_eligible(&thread, 2, 3, &allowed, 50.0));
    thread.utilization_valid = 0;
    ok &= expect("reject invalid utilization sample",
                 !candidate_thread_is_eligible(&thread, 2, 3, &allowed, 5.0));
    ok &= expect("reject identical source and destination",
                 !candidate_thread_is_eligible(&thread, 2, 2, &allowed, 5.0));

    if (!ok) return 1;
    puts("PASS: candidate eligibility, affinity constraints, utilization threshold, and invalid samples");
    return 0;
}
