#!/bin/bash
# nxr6b host test of NpLanFlag (platform/sdk/net/np_lan_flag.h) with mutants: Read() stuck at 0, Publish() inverted, Publish() dropped must each fail it.
set -eu
H=$(cd "$(dirname "$0")" && pwd); O=${1:-/tmp/np_lan_flag_test}; mkdir -p "$O"
R="$H/../../.."   # native/
b() { g++ -std=gnu++17 -O1 -g -Wall -pthread -I"$R" "$H/test_lan_flag.cpp" -o "$O/t$2"; }
b x ""; "$O/t"
m() { mkdir -p "$O/inc$1/platform/sdk/net"; sed "$2" "$R/platform/sdk/net/np_lan_flag.h" > "$O/inc$1/platform/sdk/net/np_lan_flag.h"
      g++ -std=gnu++17 -O1 -g -pthread -I"$O/inc$1" -I"$R" "$H/test_lan_flag.cpp" -o "$O/t$1"
      if "$O/t$1" >/dev/null 2>&1; then echo "MUTANT $1 SURVIVED"; exit 1; else echo "mutant $1 killed"; fi; }
m read0 's/return __atomic_load_n(Cell(), __ATOMIC_ACQUIRE) != 0;/return false;/'
m inv 's/active ? 1 : 0/active ? 0 : 1/'
m drop 's/__atomic_store_n(Cell(), active ? 1 : 0, __ATOMIC_RELEASE);//'
echo "run_lan_flag_test: ok"
