cd "/mnt/c/Users/nares/OneDrive/Documents/VAMSI/OS_PROJECT"

mkdir -p execute

# Test 1: Shared monitoring interface
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 \
    -Isrc tests/monitor_data_smoke_test.c \
    src/monitor_data.c -o execute/monitor_data_smoke_test

./execute/monitor_data_smoke_test

# Test 2: Candidate selection rules
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 \
    -Isrc tests/candidate_selector_test.c \
    src/candidate_selector.c -lm \
    -o execute/candidate_selector_test

./execute/candidate_selector_test

# Test 3: Ensure the detector still works
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 \
    -Isrc tests/imbalance_detector_test.c \
    src/imbalance_detector.c -lm \
    -o execute/imbalance_detector_test

./execute/imbalance_detector_test


---------------------------------------------------------------------------


cd "/mnt/c/Users/nares/OneDrive/Documents/VAMSI/OS_PROJECT"

mkdir -p execute

gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 \
    -Isrc tests/benefit_cost_policy_test.c \
    src/benefit_cost_policy.c -lm \
    -o execute/benefit_cost_policy_test

./execute/benefit_cost_policy_test


---------------------------------------------------------------------------


