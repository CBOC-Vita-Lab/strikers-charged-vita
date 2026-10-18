#!/bin/bash
# pltaddr.sh <exe> <symbol regex>: PLT entry address(es) of imported functions in a NON-PIE armhf exe (16-byte entries after the 32-byte PLT0)
E=$1; P=$2
GP=$(arm-linux-gnueabihf-readelf -SW $E | sed 's/\[ */[/' | awk '$2==".got.plt"{print $4}')
PL=$(arm-linux-gnueabihf-readelf -SW $E | sed 's/\[ */[/' | awk '$2==".plt"{print $4}')
arm-linux-gnueabihf-readelf -r $E | awk '/JUMP_SLOT/{print $1, $NF}' | while read slot name; do
  if echo "$name" | grep -Eq "$P"; then
    i=$(( (0x$slot - 0x$GP - 12) / 4 ))
    printf "%x %s\n" $(( 0x$PL + 32 + 16 * i )) "$name"
  fi
done
