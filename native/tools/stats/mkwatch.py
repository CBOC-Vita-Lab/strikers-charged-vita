#!/usr/bin/env python3
"""MSC_WATCH string that reads the per-player match statistics (StatsTracker::mCurrentPlayerStats[team][player], PlayerStats at
+0xEC + (team*5+player)*0x54) from the native game or from the recompiled oracle (guest memory, big-endian aware: the harness reads
u16/u32 in the guest's order).  usage: mkwatch.py native|oracle   -> prints the string for MSC_WATCH=...
Use with MSC_WATCH_EVERY large: the last [watch] line (printed when the script exits) carries the final numbers; tools/stats/cmp.py sums per team."""
import sys
FIELDS = [("shotsLow",0x00,"u16"),("shotsMed",0x02,"u16"),("shotsHigh",0x04,"u16"),("shotsOnGoal",0x06,"u16"),
          ("goalsReg",0x08,"u16"),("goals1T",0x0A,"u16"),("goalsSTS",0x0C,"u16"),("assists",0x0E,"u16"),
          ("megaTry",0x16,"u16"),("megaGoals",0x18,"u16"),("fouls",0x1A,"u16"),("powerups",0x1E,"u16"),
          ("passes",0x2A,"u16"),("received",0x30,"u16"),("hits",0x32,"u16"),("attacks",0x34,"u16"),("steals",0x36,"u16"),
          ("possess",0x3C,"u32"),("intercepted",0x4A,"u16")]
def main():
    kind = sys.argv[1] if len(sys.argv) > 1 else "native"
    inst = "_ZN11nlSingletonI12StatsTrackerE11s_pInstanceE" if kind == "native" else "806E0F58"
    out = []
    for t in range(2):
        for p in range(5):
            base = 0xEC + (t*5+p)*0x54
            for name, off, ty in FIELDS:
                out.append("st%d_%d_%s:%s:[%s]+%X" % (t, p, name, ty, inst, base+off))
    print(",".join(out))
main()
