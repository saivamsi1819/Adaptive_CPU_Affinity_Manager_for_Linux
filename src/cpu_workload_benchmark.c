#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_WORKERS 64U
#define MAX_BASE_ITERATIONS UINT64_C(5000000000)
#define MAX_SKEW_PERCENT 900U

typedef struct {
    pthread_mutex_t *mutex;
    pthread_cond_t *condition;
    int start;
    size_t index;
    uint64_t iterations;
    uint64_t checksum;
} Worker;

static int parse_u64(const char *text, uint64_t minimum, uint64_t maximum,
                     uint64_t *value)
{
    if (text == NULL || value == NULL || *text == '\0')
        return -1;

    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < minimum ||
        parsed > maximum)
        return -1;

    *value = (uint64_t)parsed;
    return 0;
}

static double elapsed_seconds(struct timespec start, struct timespec end)
{
    return (double)(end.tv_sec - start.tv_sec) +
           (double)(end.tv_nsec - start.tv_nsec) / 1e9;
}

static void *run_worker(void *argument)
{
    Worker *worker = argument;
    pthread_mutex_lock(worker->mutex);
    while (!worker->start)
        pthread_cond_wait(worker->condition, worker->mutex);
    pthread_mutex_unlock(worker->mutex);

    uint64_t state = UINT64_C(0x9e3779b97f4a7c15) ^
                     ((uint64_t)worker->index * UINT64_C(0xd6e8feb86659fd93));
    for (uint64_t i = 0; i < worker->iterations; ++i) {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        state *= UINT64_C(0x2545f4914f6cdd1d);
    }
    worker->checksum = state;
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        printf("Usage: %s WORKERS BASE_ITERATIONS SKEW_PERCENT\n"
               "WORKERS 1..%u; BASE_ITERATIONS 1000..%" PRIu64
               "; SKEW_PERCENT 0..%u extra work for worker 0.\n",
               argv[0], MAX_WORKERS, MAX_BASE_ITERATIONS,
               MAX_SKEW_PERCENT);
        return 0;
    }

    if (argc != 4) {
        fprintf(stderr,
                "Usage: %s WORKERS BASE_ITERATIONS SKEW_PERCENT\n"
                "  WORKERS: 1..%u; each worker performs the same fixed CPU loop\n"
                "  BASE_ITERATIONS: 1000..%" PRIu64 " per worker\n"
                "  SKEW_PERCENT: 0..%u extra work for worker 0\n",
                argv[0], MAX_WORKERS, MAX_BASE_ITERATIONS,
                MAX_SKEW_PERCENT);
        return 2;
    }

    uint64_t parsed_workers = 0;
    uint64_t base_iterations = 0;
    uint64_t skew_percent = 0;
    if (parse_u64(argv[1], 1, MAX_WORKERS, &parsed_workers) != 0 ||
        parse_u64(argv[2], 1000, MAX_BASE_ITERATIONS,
                  &base_iterations) != 0 ||
        parse_u64(argv[3], 0, MAX_SKEW_PERCENT, &skew_percent) != 0) {
        fprintf(stderr, "Invalid benchmark arguments. Use --help for limits.\n");
        return 2;
    }

    size_t worker_count = (size_t)parsed_workers;
    pthread_t threads[MAX_WORKERS];
    Worker workers[MAX_WORKERS] = {0};
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t condition = PTHREAD_COND_INITIALIZER;

    for (size_t i = 0; i < worker_count; ++i) {
        workers[i].mutex = &mutex;
        workers[i].condition = &condition;
        workers[i].index = i;
        workers[i].iterations = base_iterations;
    }
    workers[0].iterations += (base_iterations / 100U) * skew_percent;

    size_t created = 0;
    int create_error = 0;
    for (; created < worker_count; ++created) {
        create_error = pthread_create(&threads[created], NULL, run_worker,
                                      &workers[created]);
        if (create_error != 0)
            break;
    }

    struct timespec start;
    struct timespec finish;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        perror("clock_gettime");
        pthread_mutex_lock(&mutex);
        for (size_t i = 0; i < created; ++i)
            workers[i].start = 1;
        pthread_cond_broadcast(&condition);
        pthread_mutex_unlock(&mutex);
        for (size_t i = 0; i < created; ++i)
            pthread_join(threads[i], NULL);
        return 1;
    }

    pthread_mutex_lock(&mutex);
    for (size_t i = 0; i < created; ++i)
        workers[i].start = 1;
    pthread_cond_broadcast(&condition);
    pthread_mutex_unlock(&mutex);

    for (size_t i = 0; i < created; ++i)
        pthread_join(threads[i], NULL);

    if (create_error != 0) {
        fprintf(stderr, "pthread_create failed: %d\n", create_error);
        return 1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &finish) != 0) {
        perror("clock_gettime");
        return 1;
    }

    uint64_t checksum = 0;
    uint64_t completed_iterations = 0;
    for (size_t i = 0; i < worker_count; ++i) {
        checksum ^= workers[i].checksum;
        completed_iterations += workers[i].iterations;
    }

    printf("benchmark workers=%zu base_iterations=%" PRIu64
           " skew_percent=%" PRIu64 " completed_iterations=%" PRIu64
           " elapsed_seconds=%.6f checksum=%" PRIu64 "\n",
           worker_count, base_iterations, skew_percent,
           completed_iterations, elapsed_seconds(start, finish), checksum);

    pthread_cond_destroy(&condition);
    pthread_mutex_destroy(&mutex);
    return 0;
}
