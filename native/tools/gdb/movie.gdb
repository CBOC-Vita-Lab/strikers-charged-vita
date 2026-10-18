set pagination off
set confirm off
set print thread-events off
break _exit
commands
  silent
  printf "### exit: g_bActive=%d g_bFinished=%d g_uCurrentFrame=%u g_uDecodeCount=%u g_uNextDecodeFrame=%u start=%d\n", *(char*)&g_bActive, *(char*)&g_bFinished, *(unsigned*)&g_uCurrentFrame, *(unsigned*)&g_uDecodeCount, *(unsigned*)&g_uNextDecodeFrame, *(char*)&start
  printf "### SimpleControl numFrames=%u open=%d preFetch=%d loop=%d audioExist=%d nextDecode=%d readIndex=%d totalRead=%d dvdError=%d readProgress=%u curOffset=%d\n", *(unsigned*)((char*)&SimpleControl+0x18), *(int*)((char*)&SimpleControl+0x68), *(unsigned char*)((char*)&SimpleControl+0x6c), *(unsigned char*)((char*)&SimpleControl+0x6e), *(unsigned char*)((char*)&SimpleControl+0x6f), *(int*)((char*)&SimpleControl+0x7c), *(int*)((char*)&SimpleControl+0x80), *(int*)((char*)&SimpleControl+0x88), *(int*)((char*)&SimpleControl+0x74), *(unsigned*)((char*)&SimpleControl+0x78), *(int*)((char*)&SimpleControl+0x70)
  x/10wx ((char*)&SimpleControl+0x9c)
  printf "### frame count glx: %u\n", ((unsigned(*)(void))glxGetFrameCount)()
  kill
  quit
end
run
