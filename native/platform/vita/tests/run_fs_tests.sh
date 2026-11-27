#!/bin/bash
# The frame-send worker's unit tests in its four modes.   RUN="<qemu-arm ...>" BIN=<test_framesend> run_fs_tests.sh
BIN=${BIN:?}; RUN=${RUN:-}
fail=0
for m in thread serial inline off; do
  if env MSC_FRAMESEND=$m $RUN "$BIN" > /tmp/np_fs_test.$$ 2>&1; then echo "ok   $m: $(grep -E '^[0-9]+ checks' /tmp/np_fs_test.$$)"; else echo "FAIL $m"; tail -8 /tmp/np_fs_test.$$; fail=1; fi
done
for fg in 1 2; do for m in thread serial; do   # MSC_FS_FREEGATE=1: a free of an old block joins; =2: it is kept until the join (the default)
  if env MSC_FRAMESEND=$m MSC_FS_FREEGATE=$fg $RUN "$BIN" > /tmp/np_fs_test.$$ 2>&1; then echo "ok   $m freegate=$fg: $(grep -E '^[0-9]+ checks' /tmp/np_fs_test.$$)"; else echo "FAIL $m freegate=$fg"; tail -8 /tmp/np_fs_test.$$; fail=1; fi
done; done
for m in thread serial; do   # the two-stage pipeline (W5 PIPE): deferred table stores
  if env MSC_FRAMESEND=$m MSC_FS_PIPE=1 $RUN "$BIN" > /tmp/np_fs_test.$$ 2>&1; then echo "ok   $m pipe: $(grep -E '^[0-9]+ checks' /tmp/np_fs_test.$$)"; else echo "FAIL $m pipe"; tail -8 /tmp/np_fs_test.$$; fail=1; fi
done
# the helper lane (MSC_SKIN_LANE=1): plain; with the Vita's kernel-semaphore maximum emulated (MSC_SEM_MAX1=1: a second post is dropped; the first lane hung on
# that); with a lane thread that does not run for 300 ms (MSC_LANE_FREEZE_MS: the game thread must run the queue itself, bounded); in the pipeline mode too
for cfg in "plain:" "semmax1:MSC_SEM_MAX1=1" "frozen:MSC_LANE_FREEZE_MS=300" "frozen+semmax1:MSC_LANE_FREEZE_MS=300 MSC_SEM_MAX1=1" "pipe+semmax1:MSC_FS_PIPE=1 MSC_SEM_MAX1=1"; do
  name=${cfg%%:*}; vars=${cfg#*:}
  if env MSC_FRAMESEND=thread MSC_SKIN_LANE=1 $vars timeout 120 $RUN "$BIN" > /tmp/np_fs_test.$$ 2>&1; then echo "ok   helper lane $name: $(grep -E '^[0-9]+ checks' /tmp/np_fs_test.$$); $(grep -E '^lane stress: [0-9]+ rounds' /tmp/np_fs_test.$$)"; else echo "FAIL helper lane $name"; tail -8 /tmp/np_fs_test.$$; fail=1; fi
done
rm -f /tmp/np_fs_test.$$
exit $fail
