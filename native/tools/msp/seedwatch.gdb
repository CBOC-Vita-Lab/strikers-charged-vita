# MSP: who draws from the simulation's random seed (gNetworkRandomSeed)? Arms a hardware watchpoint at the first fixed simulation step, logs the
# new value and the callers at every write. Used by tools/msp/job_seedwatch.sh (B and C at 30 fps, then seedwatch_diff.py finds the first
# draw that exists in one run only).
set pagination off
set confirm off
break FixedUpdateTask::CallFixedUpdateTasks
run
delete 1
watch *(unsigned int *)&gNetworkRandomSeed
commands
silent
printf "HIT %08x\n", *(unsigned int *)&gNetworkRandomSeed
bt 5
continue
end
continue
