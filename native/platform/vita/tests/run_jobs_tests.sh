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
rm -f /tmp/np_jobs_test.$$
exit $fail
