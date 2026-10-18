#!/usr/bin/env python3
"""W5-SEED: nav scripts that walk the real menus (Domination Mode as platform/vita/session/nav/match60.txt) and pick a STADIUM on the way, then play a little.

  gen_nav.py --out DIR probe        probes: which pad button pages the stadium carousel (R, RIGHT, L) -> DIR/stadprobe_*.txt
  gen_nav.py --out DIR stadium BTN N TAG [FRAMES]   DIR/stad_<TAG>.txt: press BTN (hex) N times in the carousel, play FRAMES frames (default 900)

The carousel (src/Game/SH/SHStadiumSelect.cpp) shows STADIUM_ORDER = {13, 11, 15, 16, 7, 14, 3, 4, 9, 5, 0, 1, 2, 6, 10, 8, 12}: index k -> stadium id; "next" is
index+1. The match prints `stad=<id>` in its [watch] lines (host32) and the game log of a Vita3K run shows which stadium loaded.
"""
import argparse, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
MATCH60 = os.path.join(HERE, '../../platform/vita/session/nav/match60.txt')
sys.path.insert(0, HERE)
from gen_scripts import monkey


def head():
    L = open(MATCH60).read().split('\n')
    end = next(i for i, l in enumerate(L) if l.startswith('# Stadium: the default one'))
    return L[:end]


def stadium_script(btn, n, tag, frames=900):
    L = head()
    L += ['# W5-SEED: stadium carousel: press %x %d times' % (btn, n),
          'wait onoff == 0x110', 'idle 90']
    for i in range(n):
        L += ['press %x 3 20' % btn, 'idle 40']
    L += ['log STADIUM carousel stepped %d times with %x' % (n, btn), 'idle 30', 'press 100', 'log stadium confirmed, loading the match',
          'wait ingame == 1', 'mash 100 30 gs == 1', 'log STATE kick-off', 'wait gs == 5', 'log STATE open play']
    L += monkey(9000 + n, frames)
    L += ['log STATE script end', 'exit']
    return '\n'.join(L) + '\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(HERE, 'nav'))
    ap.add_argument('cmd')
    ap.add_argument('rest', nargs='*')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    if a.cmd == 'probe':
        for nm, b, n in (('R2', 0x20, 2), ('RIGHT2', 0x2, 2), ('L1', 0x40, 1)):
            open(os.path.join(a.out, 'stadprobe_%s.txt' % nm), 'w').write(stadium_script(b, n, nm, 300))
            print('stadprobe_%s.txt' % nm)
    elif a.cmd == 'stadium':
        b, n, tag = int(a.rest[0], 16), int(a.rest[1]), a.rest[2]
        fr = int(a.rest[3]) if len(a.rest) > 3 else 900
        p = os.path.join(a.out, 'stad_%s.txt' % tag)
        open(p, 'w').write(stadium_script(b, n, tag, fr))
        print(p)


if __name__ == '__main__':
    main()
