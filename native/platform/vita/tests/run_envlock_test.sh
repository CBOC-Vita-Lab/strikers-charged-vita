#!/bin/bash
# Host test of np_vita_envlock.cpp (the __env_lock / __env_unlock of the Vita build). Usage: run_envlock_test.sh [outdir]
# The control: the same test against a lock that does nothing (the SDK's empty one) must FAIL.
set -eu
H=$(cd "$(dirname "$0")" && pwd); O=${1:-/tmp/np_envlock_test}; mkdir -p "$O"
F="-std=gnu++17 -O1 -g -pthread -DNP_ENVLOCK_HOST"
g++ $F "$H/test_envlock.cpp" "$H/../src/np_vita_envlock.cpp" -o "$O/t_real"
"$O/t_real"
cat > "$O/empty_lock.cpp" <<'EOC'
struct _reent;
extern "C" { void __env_lock(struct _reent*) {} void __env_unlock(struct _reent*) {} void np_env_lock_stats(unsigned*, unsigned*, unsigned*) {} }
EOC
g++ $F "$H/test_envlock.cpp" "$O/empty_lock.cpp" -o "$O/t_empty"
if "$O/t_empty" > "$O/empty.out" 2>&1; then echo "run_envlock_test: THE CONTROL (empty lock) PASSED: the test cannot fail"; exit 1; fi
echo "run_envlock_test: ok (the empty lock fails: $(grep -c FAIL "$O/empty.out") checks)"
