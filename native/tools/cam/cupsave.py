#!/usr/bin/env python3
"""W5-CAM: read or set the cup-won bits of a Strikers2 save (Wii format, the native build's own), python3 -I.

    cupsave.py show <Strikers2>                 the 9 cup unlock bits (1 Fire Cup, 2 Crystal Cup, 4 Striker Cup: the three captains, their stadiums,
                                                the cup trophies; 8|16, 32|64, 128|256: the goal trophies of each cup = Wastelands, Dump, Galactic)
    cupsave.py set <in> <out> <bits>            out = in with those bits ORed in, header CRC made again (a save whose cups were won)

Offsets as platform/sdk/tests/test_save_import.cpp: payload at 8, end 8 + 35578, the 928-byte cup progress span before the last 52 bytes,
CupProgressRecord::mUnlockFlags = the top 9 bits of the big-endian u16 at +44 of the span.
"""
import struct
import sys
import zlib

END = 8 + 35578
PROGRESS = END - 52 - 928
OFF = PROGRESS + 44


def flags(b):
    return struct.unpack(">H", b[OFF:OFF + 2])[0] >> 7


def sound(b):
    return len(b) == 35616 and struct.unpack(">I", b[:4])[0] == 35586 and struct.unpack(">I", b[4:8])[0] == zlib.crc32(b[8:]) & 0xffffffff


def main(argv):
    if argv[1] == "show":
        b = open(argv[2], "rb").read()
        f = flags(b)
        print("%s: sound=%s cup unlock bits 0x%03x (Fire %d, Crystal %d, Striker %d; goal trophies 0x%03x)"
              % (argv[2], sound(b), f, f & 1, (f >> 1) & 1, (f >> 2) & 1, f & 0x1f8))
        return 0
    if argv[1] == "set":
        b = bytearray(open(argv[2], "rb").read())
        assert sound(bytes(b)), "input is not a sound save"
        v = struct.unpack(">H", b[OFF:OFF + 2])[0]
        v |= (int(argv[4], 0) & 0x1ff) << 7
        b[OFF:OFF + 2] = struct.pack(">H", v)
        b[4:8] = struct.pack(">I", zlib.crc32(bytes(b[8:])) & 0xffffffff)
        open(argv[3], "wb").write(b)
        print("wrote %s: cup unlock bits 0x%03x" % (argv[3], flags(b)))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
