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
if env MSC_FRAMESEND=thread MSC_SKIN_LANE=1 $RUN "$BIN" > /tmp/np_fs_test.$$ 2>&1; then echo "ok   helper lane: $(grep -E '^[0-9]+ checks' /tmp/np_fs_test.$$)"; else echo "FAIL helper lane"; tail -8 /tmp/np_fs_test.$$; fail=1; fi
rm -f /tmp/np_fs_test.$$
exit $fail
