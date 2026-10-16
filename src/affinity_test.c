#define _GNU_SOURCE

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_affinity(const cpu_set_t *set)
{
    int first = 1;

    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (CPU_ISSET(cpu, set)) {
            printf("%s%d", first ? "" : ",", cpu);
            first = 0;
        }
    }

    if (first) {
        printf("(none)");
    }
    putchar('\n');
}

static int parse_pid(const char *text, pid_t *pid_out)
{
    if (text == NULL || *text == '\0') {
        return -1;
    }

    errno = 0;
    char *end = NULL;
    long value = strtol(text, &end, 10);

    if (errno != 0 || end == text || *end != '\0' ||
        value <= 0 || value > 2147483647L) {
        return -1;
    }

    *pid_out = (pid_t)value;
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <PID>\n", argv[0]);
        return EXIT_FAILURE;
    }

    pid_t pid;
    if (parse_pid(argv[1], &pid) != 0) {
        fprintf(stderr, "Error: invalid PID '%s'.\n", argv[1]);
        return EXIT_FAILURE;
    }

    cpu_set_t original, single_cpu, verified;
    CPU_ZERO(&original);

    if (sched_getaffinity(pid, sizeof(original), &original) == -1) {
        fprintf(stderr, "sched_getaffinity(PID %ld) failed: %s\n",
                (long)pid, strerror(errno));
        return EXIT_FAILURE;
    }

    if (CPU_COUNT(&original) == 0) {
        fprintf(stderr, "Target process has an empty allowed CPU set.\n");
        return EXIT_FAILURE;
    }

    printf("Target PID: %ld\n", (long)pid);
    printf("Original allowed CPUs: ");
    print_affinity(&original);

    /*
     * Choose the first allowed CPU. The kernel's returned affinity mask
     * already reflects the target's permitted CPU set.
     */
    int chosen_cpu = -1;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (CPU_ISSET(cpu, &original)) {
            chosen_cpu = cpu;
            break;
        }
    }

    if (chosen_cpu < 0) {
        fprintf(stderr, "Could not find an allowed CPU.\n");
        return EXIT_FAILURE;
    }

    CPU_ZERO(&single_cpu);
    CPU_SET(chosen_cpu, &single_cpu);

    printf("Temporarily restricting PID %ld to CPU %d...\n",
           (long)pid, chosen_cpu);

    if (sched_setaffinity(pid, sizeof(single_cpu), &single_cpu) == -1) {
        fprintf(stderr, "sched_setaffinity failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    /*
     * Once affinity has changed, every later error path attempts restoration.
     * This reduces the risk of accidentally leaving the target restricted.
     */
    int exit_status = EXIT_SUCCESS;
    CPU_ZERO(&verified);

    if (sched_getaffinity(pid, sizeof(verified), &verified) == -1) {
        fprintf(stderr, "Verification failed: %s\n", strerror(errno));
        exit_status = EXIT_FAILURE;
    } else {
        printf("Verified allowed CPUs: ");
        print_affinity(&verified);

        if (CPU_COUNT(&verified) == 1 &&
            CPU_ISSET(chosen_cpu, &verified)) {
            printf("SUCCESS: Affinity change verified.\n");
        } else {
            fprintf(stderr,
                    "WARNING: Actual affinity differs from expectation.\n");
            exit_status = EXIT_FAILURE;
        }
    }

    if (sched_setaffinity(pid, sizeof(original), &original) == -1) {
        fprintf(stderr, "WARNING: Could not restore original affinity: %s\n",
                strerror(errno));
        fprintf(stderr,
                "If the target still exists, inspect it with taskset -pc %ld\n",
                (long)pid);
        return EXIT_FAILURE;
    }

    printf("Original affinity restored.\n");
    return exit_status;
}
