# MSP: force the AI script time budget (gScriptTimeBudget[0]) to the value in $nb whenever the game writes it, so a single-player match runs with
# the budget a network or recorded match uses (-1: no script is ever skipped, no draw from nlDefaultSeed). Used by tools/msp/job_pace5.sh:
#   gdb -q -batch -ex 'set $nb = -1.0' -x tools/msp/budget.gdb <exe>
set pagination off
set confirm off
watch *(float *)&gScriptTimeBudget
commands
silent
printf "BUDGET %f -> %f\n", *(float *)&gScriptTimeBudget, $nb
set {float}&gScriptTimeBudget = $nb
continue
end
run
