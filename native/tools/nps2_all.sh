#!/bin/bash
# NPS2: one heavy.sh slot for everything that needs the game: build (-full-null, host32), a gdb-sampled virtual-time run (diagnostic),
# real-time vs virtual-time runs of a nav script, menu navigation test on real objects, end-to-end save test.
#   heavy.sh -t 1700 tools/nps2_all.sh
cd "$(dirname "$0")/.."
echo "=== build + virtual-time sample run $(date +%T)"; tools/nps2_vthang.sh "${1:-tools/nav/boot_title.txt}"; echo "--- gdb samples:"; grep -E "^=== sample|g_ns|g_tickNs|g_reads|^#[0-4] " /home/claude/msc/build/np/NPS2x/vthang.gdb | head -60
echo "=== smoke $(date +%T)"; NOBUILD=1 tools/nps2_smoke.sh "${1:-tools/nav/boot_title.txt}" "${2:-1200}"
echo "=== menutest $(date +%T)"; tools/nps2_menutest.sh
echo "=== savetest $(date +%T)"; tools/nps2_savetest.sh
echo "=== done $(date +%T)"
