#!/bin/bash
# The job pool's unit tests in several configurations (lanes, orders, spin on/off, verifier).
#   RUN="<qemu-arm ...>" BIN=<test_jobs> run_jobs_tests.sh
BIN=${BIN:?}; RUN=${RUN:-}
fail=0
t() { local name=$1; shift; if env "$@" $RUN "$BIN" > /tmp/np_jobs_test.$$ 2>&1; then echo "ok   $name: $(grep -E '^[0-9]+ checks' /tmp/np_jobs_test.$$)"; else echo "FAIL $name"; tail -8 /tmp/np_jobs_test.$$; fail=1; fi; }
t "serial (1 lane)"            MSC_JOBS=1
t "2 lanes"                    MSC_JOBS=2
t "3 lanes"                    MSC_JOBS=3
t "3 lanes, never spin"        MSC_JOBS=3 MSC_JOBS_SPIN_US=0
t "3 lanes, reverse order"     MSC_JOBS=3 MSC_JOBS_ORDER=reverse
t "3 lanes, shuffled order"    MSC_JOBS=3 MSC_JOBS_ORDER=shuffle
t "3 lanes, interleaved"       MSC_JOBS=3 MSC_JOBS_ORDER=interleave
t "3 lanes, verifier"          MSC_JOBS=3 MSC_JOBS_VERIFY=1 MSC_JOBS_SPIN_US=2000
t "4 lanes"                    MSC_JOBS=4
t "3 lanes, timed re-check"    MSC_JOBS=3 MSC_JOBS_SPIN_US=0 MSC_JOBS_RECHECK_MS=5
t "core 3 lane (fake core 3)"  MSC_JOBS=3 MSC_VITA_CORE3_WORK=1 NP_CORE3_FAKE=1
t "core 3 lane, shuffled"      MSC_JOBS=3 MSC_VITA_CORE3_WORK=1 NP_CORE3_FAKE=1 MSC_JOBS_ORDER=shuffle MSC_JOBS_SPIN_US=0
t "core 3 lane, verifier"      MSC_JOBS=2 MSC_VITA_CORE3_WORK=1 NP_CORE3_FAKE=1 MSC_JOBS_VERIFY=1
t "core 3 first (alias)"       MSC_JOBS=3 MSC_VITA_CORE3_WORK=1 NP_CORE3_FAKE=1 MSC_JOBS_ORDER=core3first
t "core 3 first off"           MSC_JOBS=3 MSC_VITA_CORE3_WORK=1 NP_CORE3_FAKE=1 MSC_JOBS_C3FIRST=0
t "core 3 front order (C3FIRST=2)" MSC_JOBS=3 MSC_VITA_CORE3_WORK=1 NP_CORE3_FAKE=1 MSC_JOBS_C3FIRST=2
t "core 3 front order, given up" MSC_JOBS=3 MSC_VITA_CORE3_WORK=1 NP_CORE3_FAKE=1 MSC_JOBS_C3FIRST=2 MSC_VITA_CORE3_SLOW_US=1 TEST_C3F_RETIRE=1
t "core 3 first, verifier"     MSC_JOBS=3 MSC_VITA_CORE3_WORK=1 NP_CORE3_FAKE=1 MSC_JOBS_VERIFY=1 MSC_JOBS_SPIN_US=2000
t "core 3 first, given up"     MSC_JOBS=3 MSC_VITA_CORE3_WORK=1 NP_CORE3_FAKE=1 MSC_VITA_CORE3_SLOW_US=1 TEST_C3F_RETIRE=1
t "core 3 first, no lane"      MSC_JOBS=3
t "core 3 asked, not proven"   MSC_JOBS=3 MSC_VITA_CORE3_WORK=1
rm -f /tmp/np_jobs_test.$$
exit $fail
