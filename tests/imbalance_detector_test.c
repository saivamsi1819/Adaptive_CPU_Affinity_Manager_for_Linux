#define _POSIX_C_SOURCE 200809L
#include "imbalance_detector.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return EXIT_FAILURE; \
    } \
} while (0)

static struct timespec at_seconds(long seconds)
{
    struct timespec value = { .tv_sec = seconds, .tv_nsec = 0 };
    return value;
}

static CpuUtilizationSummary sample(double imbalance, int valid)
{
    CpuUtilizationSummary value = {0};
    value.valid = valid;
    value.valid_cpu_count = valid ? 4U : 0U;
    value.imbalance_percent_points = imbalance;
    return value;
}

int main(void)
{
    ImbalanceDetector detector;
    ImbalanceDetectorConfig config = imbalance_detector_default_config();
    CHECK(imbalance_detector_init(&detector, &config) == 0);

    CpuUtilizationSummary invalid = sample(80.0, 0);
    CpuUtilizationSummary high = sample(35.0, 1);
    CpuUtilizationSummary middle = sample(25.0, 1);
    CpuUtilizationSummary low = sample(15.0, 1);

    CHECK(imbalance_detector_update(&detector, &invalid, at_seconds(0)) ==
          IMBALANCE_RESULT_INVALID_SAMPLE);
    CHECK(detector.consecutive_imbalanced_samples == 0);

    CHECK(imbalance_detector_update(&detector, &high, at_seconds(1)) ==
          IMBALANCE_RESULT_PERSISTING);
    CHECK(imbalance_detector_update(&detector, &invalid, at_seconds(2)) ==
          IMBALANCE_RESULT_INVALID_SAMPLE);
    CHECK(detector.consecutive_imbalanced_samples == 0);
    CHECK(imbalance_detector_update(&detector, &high, at_seconds(3)) ==
          IMBALANCE_RESULT_PERSISTING);
    CHECK(imbalance_detector_update(&detector, &high, at_seconds(4)) ==
          IMBALANCE_RESULT_PERSISTING);
    CHECK(imbalance_detector_update(&detector, &high, at_seconds(5)) ==
          IMBALANCE_RESULT_DETECTED);

    /* Hysteresis: 25 pp is below entry but above exit, so active remains. */
    CHECK(imbalance_detector_update(&detector, &middle, at_seconds(6)) ==
          IMBALANCE_RESULT_COOLDOWN);
    CHECK(detector.imbalance_active == 1);

    /* Cooldown expires at t=10; sustained imbalance can trigger again. */
    CHECK(imbalance_detector_update(&detector, &high, at_seconds(10)) ==
          IMBALANCE_RESULT_DETECTED);

    CHECK(imbalance_detector_update(&detector, &low, at_seconds(11)) ==
          IMBALANCE_RESULT_BALANCED);
    CHECK(detector.imbalance_active == 0);
    CHECK(detector.consecutive_imbalanced_samples == 0);

    CHECK(imbalance_detector_update(&detector, &high, at_seconds(12)) ==
          IMBALANCE_RESULT_PERSISTING);
    CHECK(imbalance_detector_update(&detector, &high, at_seconds(13)) ==
          IMBALANCE_RESULT_PERSISTING);
    CHECK(imbalance_detector_update(&detector, &high, at_seconds(14)) ==
          IMBALANCE_RESULT_COOLDOWN);
    CHECK(imbalance_detector_update(&detector, &high, at_seconds(15)) ==
          IMBALANCE_RESULT_DETECTED);

    config.exit_threshold_percent_points = config.enter_threshold_percent_points;
    CHECK(imbalance_detector_init(&detector, &config) == -1);

    puts("PASS: persistence, invalid samples, hysteresis, cooldown, and config validation");
    return EXIT_SUCCESS;
}
