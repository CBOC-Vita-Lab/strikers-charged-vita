#!/bin/bash
# Builds shc.vpk (title SHCC00001), see shc.c. usage: build.sh <vitaGL prefix with lib/, for example /out/v13-x/gl/prefix> <out dir>
set -eu
export VITASDK=${VITASDK:-/opt/vitasdk} PATH=/opt/vitasdk/bin:$PATH
P=$1; O=$2; H=$(cd "$(dirname "$0")" && pwd); mkdir -p "$O"
INC=(-I"$H/../../../dependencies/vitaGL/source" -I"$P/vitashark" -I"$P/include")
arm-vita-eabi-gcc -O2 -Wl,-q -std=gnu11 -w "${INC[@]}" "$H/shc.c" -o "$O/shc.elf" -L"$P/lib" -lvitaGL -lvitashark -lSceShaccCgExt -lmathneon -ltaihen_stub -lSceShaccCg_stub -lSceGxm_stub -lSceDisplay_stub -lSceCommonDialog_stub -lSceAppMgr_stub -lSceSysmodule_stub \
  -lSceKernelDmacMgr_stub -lSceRazorHud_stub -lSceRazorCapture_stub -lSceCtrl_stub -lSceTouch_stub -lSceAppUtil_stub -lSceIme_stub -lstdc++ -lm -lc -pthread
vita-elf-create "$O/shc.elf" "$O/shc.velf" >/dev/null
vita-make-fself -na "$O/shc.velf" "$O/eboot.bin"
vita-mksfoex -s TITLE_ID=SHCC00001 -d ATTRIBUTE2=12 "shader counts" "$O/param.sfo"
vita-pack-vpk -s "$O/param.sfo" -b "$O/eboot.bin" "$O/shc.vpk"
ls -la "$O/shc.vpk"
