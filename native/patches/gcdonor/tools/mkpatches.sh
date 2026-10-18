#!/bin/bash
# Regenerate patches/gcdonor/<path>.patch from the authoring branch and test them.
#   tools/mkpatches.sh [AUTHOR_BRANCH]     (default NPD-scratch; run from anywhere)
# Authoring happens in a separate worktree (/home/claude/msc/npw/NPD-scratch, branch NPD-scratch) that
# is kept rebased on `main`; a patch is the three-dot diff main...AUTHOR for one path, so it applies to
# main's tip with `git apply` (also checked against a clean index of main, and against the working tree
# of /home/claude/msc/np, which is where NPB integrates, read-only).
set -e
REPO=/home/claude/msc/np
OUT=/home/claude/msc/npw/NPD/patches/gcdonor
BR=${1:-NPD-scratch}
cd "$REPO"
base=$(git merge-base main "$BR")
tip=$(git rev-parse --short main)
find "$OUT" -name "*.patch" -delete; rm -f "$OUT"/status.tsv
tmpidx=$(mktemp)
fail=0
: > "$OUT/series"
git diff --name-status "$base" "$BR" | while read st path; do
  p="$OUT/$path.patch"
  mkdir -p "$(dirname "$p")"
  git diff --binary "$base" "$BR" -- "$path" > "$p"
  rm -f "$tmpidx"; GIT_INDEX_FILE=$tmpidx git read-tree main
  m=ok; GIT_INDEX_FILE=$tmpidx git apply --cached --check "$p" 2>/dev/null || m=FAIL
  w=ok; git apply --check "$p" 2>/dev/null || w=FAIL
  echo "$path.patch" >> "$OUT/series"
  printf '%s\t%s\t%s\t%s\n' "$path" "$st" "$m" "$w" >> "$OUT/status.tsv"
done
rm -f "$tmpidx"
echo "main=$tip base=$(git rev-parse --short $base) patches=$(wc -l < $OUT/series)"
awk -F'\t' '{print $3"/"$4"\t"$1}' "$OUT/status.tsv" | sort | uniq -c | sort -rn | head -50
