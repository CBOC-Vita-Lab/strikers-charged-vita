set pagination off
set confirm off
tbreak ModeledScreenTransition::Load
commands
  silent
  set $ra = $esp
  printf "first Load: ret slot %p holds %p\n", $ra, *(void**)$ra
  watch *(unsigned*)$ra
  commands
    printf "--- write to the return-address slot (new value %p)\n", *(void**)$ra
    bt 8
    continue
  end
  continue
end
run
bt 12
