#ifndef IMBALANCE_DETECTOR_H
#define IMBALANCE_DETECTOR_H

#include "monitor_data.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Tunable policy thresholds. Imbalance is measured in percentage points. */
typedef struct {
    double enter_threshold_percent_points;
    double exit_threshold_percent_points;
    unsigned int required_consecutive_samples;
    double cooldown_seconds;
} ImbalanceDetectorConfig;

typedef struct {
    ImbalanceDetectorConfig config;
    unsigned int consecutive_imbalanced_samples;
    int imbalance_active;
    int has_triggered;
    struct timespec last_trigger_at;
} ImbalanceDetector;

typedef enum {
    IMBALANCE_RESULT_ERROR = -1,
    IMBALANCE_RESULT_INVALID_SAMPLE = 0,
    IMBALANCE_RESULT_BALANCED = 1,
    IMBALANCE_RESULT_PERSISTING = 2,
    IMBALANCE_RESULT_COOLDOWN = 3,
    IMBALANCE_RESULT_DETECTED = 4,
    IMBALANCE_RESULT_ACTIVE = 5
} ImbalanceDetectorResult;

/* Suggested starting values, not universal optimal thresholds. */
ImbalanceDetectorConfig imbalance_detector_default_config(void);

/* Validate config and initialize detector. Returns 0 on success, -1 on error. */
int imbalance_detector_init(ImbalanceDetector *detector,
                            const ImbalanceDetectorConfig *config);

/*
 * Consume one CPU utilization summary at a CLOCK_MONOTONIC timestamp.
 * Invalid summaries do not count as samples and never trigger a decision.
 * A newly persistent imbalance returns DETECTED when cooldown permits.
 * While active, another DETECTED event may be emitted after cooldown if the
 * imbalance remains above the exit threshold. The controller must still apply
 * candidate, destination, safety, and benefit-versus-cost checks.
 */
ImbalanceDetectorResult imbalance_detector_update(
    ImbalanceDetector *detector,
    const CpuUtilizationSummary *summary,
    struct timespec sampled_at);

#ifdef __cplusplus
}
#endif

#endif /* IMBALANCE_DETECTOR_H */
