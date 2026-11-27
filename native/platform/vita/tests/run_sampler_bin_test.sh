#!/bin/bash
# gxp17: host tests of the on-console sampler's dump: np_sampler_bin.h (histogram + writer, 4 mutants that must be caught) and tools/console-1.2/sampler_report.py read on a synthetic .bin
# (written by the real writer) with a synthetic ELF (gcc, nm): expected run/wait split, load offset from the anchor, subsystem and function shares, one report mutant caught. Usage: run_sampler_bin_test.sh [outdir]
set -e
H=$(cd "$(dirname "$0")" && pwd); R=$(cd "$H/../../../.." && pwd); O=${1:-/tmp/np_sampler_bin_test}; rm -rf "$O"; mkdir -p "$O"
CXX=${CXX:-g++}
$CXX -O1 -I"$H/../src" "$H/test_sampler_bin.cpp" -o "$O/t_real"; "$O/t_real"
mut() { # name sed-expression
  mkdir -p "$O/m$1"; sed "$2" "$H/../src/np_sampler_bin.h" > "$O/m$1/np_sampler_bin.h"; cmp -s "$O/m$1/np_sampler_bin.h" "$H/../src/np_sampler_bin.h" && { echo "mutant $1 did not change the header"; exit 1; }
  $CXX -O1 -I"$O/m$1" "$H/test_sampler_bin.cpp" -o "$O/t_m$1"
  if "$O/t_m$1" >/dev/null; then echo "MUTANT $1 NOT CAUGHT"; exit 1; else echo "mutant $1 caught (expected)"; fi
}
mut 1 's/e.lr == lr \&\& //; s/ ^ (lr \* 0x85ebca6bu)//'
mut 2 's/used_ >= cap_ - cap_ \/ 8/used_ >= cap_ - 1/'
mut 3 's/(kind \& 1u) << 8/(kind \& 1u) << 9/'
mut 4 's/written == h.nEntries/written + 1 == h.nEntries/; s/++written; }/++written; break; }/'
# ---- the report: a tiny ELF with three functions at known addresses
cat > "$O/fake.c" <<'EOC'
void msc_sampler_anchor(void) {}
int Game_Hot(int x) { return x * 3 + 1; }
int Game_Caller(int x) { return Game_Hot(x) + 2; }
int main(void) { return Game_Caller(0) & 0; }
EOC
gcc -O0 -no-pie -fno-inline -o "$O/fake.elf" "$O/fake.c"
addr() { nm "$O/fake.elf" | awk -v n="$1" '$3==n{print $1}'; }
A=$(addr msc_sampler_anchor); HOT=$(addr Game_Hot); CAL=$(addr Game_Caller)
OFF=0x10000   # (the run-time image sits 0x10000 above the ELF)
rt() { printf '%x' $((0x$1 + OFF + ${2:-0})); }
mkdir -p "$O/src/native/src/Game/AI" "$O/src/native/src/NL"
printf 'int Game_Hot(int x) {\n return x;\n}\n' > "$O/src/native/src/Game/AI/Hot.cpp"
printf 'int Game_Caller(int x) {\n return x;\n}\n' > "$O/src/native/src/NL/Caller.cpp"
$CXX -O1 -DMAKE_BIN -I"$H/../src" "$H/test_sampler_bin.cpp" -o "$O/t_makebin"
"$O/t_makebin" "$O/sampler-1.bin" "$(rt $A)" "$(rt $HOT 4)" "$(rt $CAL 4)"
python3 -I "$R/tools/console-1.2/sampler_report.py" "$O/fake.elf" "$O/sampler-1.bin" --src "$O/src" --nm "$(command -v nm)" --top 5 > "$O/report.txt"
cat "$O/report.txt"
chk() { grep -Eq "$1" "$O/report.txt" || { echo "REPORT CHECK FAILED: $1"; exit 1; }; }
chk "load offset \+0x10000"
chk "tag synthetic"
chk "=== thread game: 1000 samples, running 800 \(80.0%\), waiting 200 \(20.0%\)"
chk "wait semaphore +200"
chk "=== thread worker: 50 samples, running 50 \(100.0%\)"
chk "stopped it 4.00%.*800 stops, avg 50 us, max 90 us"
chk "its own thread used 1.20% of one core"
chk "800 +100.0%  \[Game/AI\] Game_Hot"          # SELF: all 800 running samples in Game_Hot
chk "700 +87.5%.*\[NL\] Game_Caller"
chk "800 +100.0%  Game/AI"                                    # subsystems SELF
chk "700 +87.5%  NL"                                           # subsystems INCLUSIVE-BY-LR: the caller's subsystem is counted too                                  # INCLUSIVE-BY-LR: 700 of them have the caller in lr
# a report mutant: a wrong load offset must NOT give the same table
sed 's/delta = b0\["anchor"\] - elf_anchor/delta = 0/' "$R/tools/console-1.2/sampler_report.py" > "$O/rep_mut.py"
python3 -I "$O/rep_mut.py" "$O/fake.elf" "$O/sampler-1.bin" --src "$O/src" --nm "$(command -v nm)" > "$O/report_mut.txt" || true
if grep -Eq "800 +100.0%.*Game_Hot" "$O/report_mut.txt"; then echo "REPORT MUTANT NOT CAUGHT"; exit 1; else echo "report mutant caught (expected)"; fi
echo "sampler dump tests: all passed"
