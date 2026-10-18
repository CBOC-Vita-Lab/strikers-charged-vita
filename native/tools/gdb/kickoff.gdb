set pagination off
set confirm off
set print thread-events off
break _ZN5cGame15ResetForKickOffEv
commands
  silent
  printf "### ResetForKickOff this=%p\n", *(void**)($esp+4)
  continue
end
break PostResetCallback
commands
  silent
  printf "### PostResetCallback\n"
  bt 5
  continue
end
break fn_800227C8
commands
  silent
  printf "### fn_800227C8 (Kickoff event handler)\n"
  continue
end
break _ZN5Clock5StartEv
commands
  silent
  printf "### Clock::Start this=%p state=%d end=%f scale=%f timer=%f\n", *(void**)($esp+4), *(int*)(*(int*)($esp+4)+0x18), *(float*)(*(int*)($esp+4)+0xC), *(float*)(*(int*)($esp+4)+4), *(float*)(*(int*)($esp+4)+8)
  continue
end
break exit
commands
  silent
  printf "### exit: ClockManager active=%p pending=%p inactive=%p updating=%d\n", *(void**)&_ZN12ClockManager12m_activeListE, *(void**)&_ZN12ClockManager18m_pendingActiveListE, *(void**)&_ZN12ClockManager14m_inactiveListE, *(char*)&_ZN12ClockManager14m_bUpdatingClocksE
  kill
  quit
end
run
