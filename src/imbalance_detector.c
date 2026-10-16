#define _POSIX_C_SOURCE 200809L
#include "imbalance_detector.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

static int valid_timespec(struct timespec value)
{
    return value.tv_sec >= 0 && value.tv_nsec >= 0 && value.tv_nsec < 1000000000L;
}

static double seconds_between(struct timespec earlier, struct timespec later)
{
    return (double)(later.tv_sec - earlier.tv_sec) +
           (double)(later.tv_nsec - earlier.tv_nsec) / 1e9;
}

ImbalanceDetectorConfig imbalance_detector_default_config(void)
{
    ImbalanceDetectorConfig config;
    config.enter_threshold_percent_points = 30.0;
    config.exit_threshold_percent_points = 20.0;
    config.required_consecutive_samples = 3;
    config.cooldown_seconds = 5.0;
    return config;
}

static int valid_config(const ImbalanceDetectorConfig *config)
{
    return config != NULL &&
           isfinite(config->enter_threshold_percent_points) &&
           isfinite(config->exit_threshold_percent_points) &&
           isfinite(config->cooldown_seconds) &&
           config->enter_threshold_percent_points >= 0.0 &&
           config->enter_threshold_percent_points <= 100.0 &&
           config->exit_threshold_percent_points >= 0.0 &&
           config->exit_threshold_percent_points < config->enter_threshold_percent_points &&
           config->required_consecutive_samples > 0 &&
           config->cooldown_seconds >= 0.0;
}

int imbalance_detector_init(ImbalanceDetector *detector,
                            const ImbalanceDetectorConfig *config)
{
    if (detector == NULL) {
        errno = EINVAL;
        return -1;
    }

    ImbalanceDetectorConfig selected = config ? *config : imbalance_detector_default_config();
    if (!valid_config(&selected)) {
        errno = EINVAL;
        return -1;
    }

    memset(detector, 0, sizeof(*detector));
    detector->config = selected;
    return 0;
}

ImbalanceDetectorResult imbalance_detector_update(
    ImbalanceDetector *detector,
    const CpuUtilizationSummary *summary,
    struct timespec sampled_at)
{
    if (detector == NULL || !valid_timespec(sampled_at)) {
        errno = EINVAL;
        return IMBALANCE_RESULT_ERROR;
    }
    if (summary == NULL || !summary->valid || summary->valid_cpu_count < 2 ||
        !isfinite(summary->imbalance_percent_points) ||
        summary->imbalance_percent_points < 0.0 ||
        summary->imbalance_percent_points > 100.0) {
        /* Missing/invalid observations must not help satisfy persistence. */
        if (!detector->imbalance_active) {
            detector->consecutive_imbalanced_samples = 0;
        }
        return IMBALANCE_RESULT_INVALID_SAMPLE;
    }

    if (detector->has_triggered &&
        seconds_between(detector->last_trigger_at, sampled_at) < 0.0) {
        /* Monotonic sample time should never move backwards. Fail closed. */
        errno = EINVAL;
        return IMBALANCE_RESULT_ERROR;
    }

    const double imbalance = summary->imbalance_percent_points;

    if (detector->imbalance_active) {
        if (imbalance <= detector->config.exit_threshold_percent_points) {
            detector->imbalance_active = 0;
            detector->consecutive_imbalanced_samples = 0;
            return IMBALANCE_RESULT_BALANCED;
        }
    } else {
        if (imbalance < detector->config.enter_threshold_percent_points) {
            detector->consecutive_imbalanced_samples = 0;
            return IMBALANCE_RESULT_BALANCED;
        }

        if (detector->consecutive_imbalanced_samples < detector->config.required_consecutive_samples) {
            detector->consecutive_imbalanced_samples++;
        }
        if (detector->consecutive_imbalanced_samples < detector->config.required_consecutive_samples) {
            return IMBALANCE_RESULT_PERSISTING;
        }
        detector->imbalance_active = 1;
    }

    if (detector->has_triggered &&
        seconds_between(detector->last_trigger_at, sampled_at) < detector->config.cooldown_seconds) {
        return IMBALANCE_RESULT_COOLDOWN;
    }

    detector->last_trigger_at = sampled_at;
    detector->has_triggered = 1;
    return IMBALANCE_RESULT_DETECTED;
}
