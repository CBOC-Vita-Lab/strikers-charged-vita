#!/bin/bash
# W5 CTL: the per-line controls test scripts from the controls table (platform/sdk/input/np_control_table.h):
#   tools/ctl/gen_lines.sh   -> tools/ctl/nav/ctl_lines_{modern,modern_off,classic,classic_rear,modern_mega,classic_mega}.txt
# Run each with the matching switches (the scripts' headers say which) and MSC_ITEM_TEST=1; the log's "[script] CHECK RESULT" lines
# give every line's verdict, "[script] checks: N passed ..." the total.
set -e
here=$(cd "$(dirname "$0")" && pwd); np=$(cd "$here/../.." && pwd)
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
g++ -std=gnu++11 -O1 -Wall -Wextra -I "$np/platform/sdk/input" "$here/control_lines.cpp" "$np/platform/sdk/input/np_controls.cpp" -o "$tmp/control_lines"
"$tmp/control_lines" script modern > "$here/nav/ctl_lines_modern.txt"
"$tmp/control_lines" script modern rear=0 touch=0 > "$here/nav/ctl_lines_modern_off.txt"
"$tmp/control_lines" script classic > "$here/nav/ctl_lines_classic.txt"
"$tmp/control_lines" script classic rear=1 > "$here/nav/ctl_lines_classic_rear.txt"
# the own Mega Strike against an easy CPU (a skilled one tackles the captain during the wind-up)
"$tmp/control_lines" script modern skill=0 only=mega > "$here/nav/ctl_lines_modern_mega.txt"
"$tmp/control_lines" script classic skill=0 only=mega > "$here/nav/ctl_lines_classic_mega.txt"
for v in modern classic; do "$tmp/control_lines" list $v; done
