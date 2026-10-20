
CC ?= gcc
CPPFLAGS = -D_GNU_SOURCE -Isrc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
LDLIBS ?= -lm

SRC := src
TESTS := tests
OUT := execute

CORE := $(SRC)/monitor_data.c \
        $(SRC)/imbalance_detector.c \
        $(SRC)/candidate_selector.c \
        $(SRC)/benefit_cost_policy.c \
        $(SRC)/locality_policy.c \
        $(SRC)/affinity_lease_guardian.c

.PHONY: all clean test benchmark help

all: $(OUT)/cpu_affinity_manager \
	 $(OUT)/cpu_monitor \
	 $(OUT)/process_monitor \
	 $(OUT)/thread_monitor \
	 $(OUT)/affinity_test

$(OUT):
	mkdir -p $(OUT)

$(OUT)/cpu_affinity_manager: $(SRC)/cpu_affinity_manager.c $(CORE) | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/cpu_monitor: $(SRC)/cpu_monitor.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS)

$(OUT)/process_monitor: $(SRC)/process_monitor.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS)

$(OUT)/thread_monitor: $(SRC)/thread_monitor.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS)

$(OUT)/affinity_test: $(SRC)/affinity_test.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS)

$(OUT)/cpu_workload_benchmark: $(SRC)/cpu_workload_benchmark.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -o $@ $< $(LDLIBS)

benchmark: $(OUT)/cpu_workload_benchmark

$(OUT)/imbalance_detector_test: $(TESTS)/imbalance_detector_test.c $(SRC)/imbalance_detector.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/candidate_selector_test: $(TESTS)/candidate_selector_test.c $(SRC)/candidate_selector.c $(SRC)/benefit_cost_policy.c $(SRC)/locality_policy.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/benefit_cost_policy_test: $(TESTS)/benefit_cost_policy_test.c $(SRC)/benefit_cost_policy.c $(SRC)/candidate_selector.c $(SRC)/locality_policy.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/monitor_data_smoke_test: $(TESTS)/monitor_data_smoke_test.c $(SRC)/monitor_data.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/locality_policy_test: $(TESTS)/locality_policy_test.c $(SRC)/locality_policy.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/migration_decision_test: $(TESTS)/migration_decision_test.c $(SRC)/candidate_selector.c $(SRC)/benefit_cost_policy.c $(SRC)/locality_policy.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/affinity_lease_guardian_test: $(TESTS)/affinity_lease_guardian_test.c $(SRC)/affinity_lease_guardian.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

test: $(OUT)/imbalance_detector_test \
      $(OUT)/candidate_selector_test \
      $(OUT)/benefit_cost_policy_test \
      $(OUT)/monitor_data_smoke_test \
      $(OUT)/locality_policy_test \
      $(OUT)/migration_decision_test \
      $(OUT)/affinity_lease_guardian_test
	@set -e; for t in $^; do echo "===== $$t ====="; ./$$t; done

clean:
	rm -f $(OUT)/cpu_affinity_manager \
	      $(OUT)/cpu_monitor \
	      $(OUT)/process_monitor \
	      $(OUT)/thread_monitor \
	      $(OUT)/affinity_test \
	      $(OUT)/cpu_workload_benchmark \
	      $(OUT)/imbalance_detector_test \
	      $(OUT)/candidate_selector_test \
	      $(OUT)/benefit_cost_policy_test \
	      $(OUT)/monitor_data_smoke_test \
	      $(OUT)/locality_policy_test \
	      $(OUT)/migration_decision_test \
	      $(OUT)/affinity_lease_guardian_test

help:
	@echo 'make: build all Linux programs'
	@echo 'make test: build and run tests'
	@echo 'make benchmark: build the fixed-work CPU benchmark'
	@echo 'make clean: remove generated binaries from execute/'
