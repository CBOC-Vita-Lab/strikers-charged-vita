#!/bin/bash
# swap.sh <eboot file> : put it as ux0:app/CDXMP0001/eboot.bin (app quit first), read back and compare.
# Touches only ux0:app/CDXMP0001/eboot.bin. Sends only "quit CDXMP0001". SWAP_MAX = seconds for each of the upload and the read-back (default 70;
# q.sh sets it from the time left in its slice). Exit 0 and a line "SWAPPED eboot <sha>" only when the read-back equals the file.
f=${1:?usage: swap.sh <eboot file>}; H=${VITA_IP:?export VITA_IP=<the Vita address>}; M=${SWAP_MAX:-70}
A="ftp://$H:1337/ux0:/app/CDXMP0001"; S() { if command -v sha256sum >/dev/null; then sha256sum | cut -c1-16; else shasum -a 256 | cut -c1-16; fi; }
[ -s "$f" ] || { echo "NO FILE $f"; exit 1; }
(printf 'quit CDXMP0001\n'; sleep 2) | nc -w 5 "$H" 1338 >/dev/null; sleep 2
curl -sS --max-time "$M" -T "$f" "$A/eboot.bin" || { echo "UPLOAD FAILED"; exit 1; }
a=$(S < "$f"); b=$(curl -sS --max-time "$M" "$A/eboot.bin" | S); [ "$a" = "$b" ] && echo "SWAPPED eboot $a ($(wc -c < "$f") bytes)" || { echo "MISMATCH $a $b"; exit 1; }
