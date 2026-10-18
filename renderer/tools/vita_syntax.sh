#!/bin/bash
# vsyn.sh [file-stems...]: syntax-check renderer sources with the Vita build's flags (vitaGL headers).
# Since the 9 Oct merge: checks the sources of the checkout this script lives in (VGX=<checkout> overrides),
# and by default also the Vita host file vita/vgx_host_vita.cpp.
VGX=${VGX:-$(dirname "$(dirname "$(readlink -f "$0")")")}
SP=${TMPDIR:-/tmp}/vgx_vita_syntax_$(basename "$VGX"); mkdir -p $SP; cd $SP
[ $# = 0 ] && set -- $(cd $VGX/src; ls vgx_*.cpp | grep -v "vgx_host\|backend_null" | sed 's/\.cpp//') vgx_host_vita
rc=0
for f in "$@"; do
  cmd=$(make -f /home/claude/msc/vita/Makefile.vita VGX=$VGX -n obj/gl/vgx/$f.o 2>/dev/null | grep "clang++" | head -1)
  [ -z "$cmd" ] && { echo "no command for $f"; rc=1; continue; }
  cmd2=$(echo "$cmd" | sed -e 's/^@//' -e 's/-MMD//' -e 's/ -c \([^ ]*\) -o [^ ]*/ -fsyntax-only \1/' -e 's/ 2> .*//')
  if ! bash -c "$cmd2" > $f.out 2>&1; then echo "FAIL $f"; grep -m8 "error" $f.out; rc=1; fi
done
echo "vita syntax check: rc=$rc ($# files)"
exit $rc
