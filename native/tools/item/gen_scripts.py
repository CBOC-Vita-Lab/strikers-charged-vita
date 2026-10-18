#!/usr/bin/env python3
"""W5-ITEM: writes the item nav scripts (tools/item/scripts/<name>.txt) and runs.txt (name script maxframes).

Every script: boot to the title, `launch` a friendly match with pad 0 as the home team, then for ONE item:
  open   give it (harness `item`, MSC_ITEM_TEST=1), walk at the nearest opponent (`aim opp`), press X (GameCube item button) until used
  goal   walk at the goal the team attacks, give it again, face the other keeper and press X while the stick points at him
  cycle  give it + a mushroom (add), Z (GameCube cycle) puts the mushroom in front, X uses it, X uses the item
The [item] lines of the hook (slots, objects, hits, effects) and the `log ITEM ...` markers are what tools/item/report.py reads.

Sets:
  it_<item>[_<n>]   the nine items (x1, and x3 / x5 where the game deals them)
  gi_<item>         the giant (large) form: the GIANT power-up cheat (rule 4: a single item is never medium) + item x1
  cap_<captain>     the captain's Super Ability (home captain = that captain, `item me captain`)
  nat_<n>           natural acquisition: no hook, an AI match where pad 0 keeps hitting / sliding into the ball carrier
  inf_<n>           the INFINITE cheat (an item every 7.5 s to both teams), pad 0 uses and cycles them
"""
import argparse, os

STADIUMS = ['battledome', 'bowserstadium', 'craterfield', 'crystalcanyon', 'dump', 'galacticstadium', 'kongacoliseum', 'lavapit', 'pipelinecentral',
            'stormship', 'thepalace', 'thunderisland', 'underground', 'vice', 'wastelands', 'sandtomb']
CAPTAINS = ['mario', 'bowser', 'daisy', 'donkeykong', 'luigi', 'peach', 'waluigi', 'wario', 'yoshi', 'bowserjr', 'diddykong', 'petey']
ITEMS = ['green', 'red', 'spiny', 'freeze', 'banana', 'bobomb', 'chomp', 'mushroom', 'star']
ITEM_ID = {n: i for i, n in enumerate(ITEMS)}
MULTI = {'green': [3, 5], 'red': [3], 'banana': [3, 5], 'bobomb': [3, 5], 'freeze': [3], 'spiny': [3]}

PRE = """# W5-ITEM generated script: {what}
# A dismisses the wrist-strap screen and skips the intro movie; the title follows.
mash 100 20 scene == 0
log STATE title screen
idle 30
{launch}
wait ingame == 1 4000
log STATE match object
mash 100 30 gs == 1 4000
log STATE kick-off
wait gs == 5 6000
log STATE open play
idle 20
"""
POST = """log STATE script end
idle 60
exit
"""


def launch(stadium, home, away, **kw):
    s = 'launch mode=friendly stadium=%d home=%d away=%d hsk=0,1,2 ask=5,6,7 human=0 time=300 skill=%d' % (stadium, home, away, kw.pop('skill', 0))
    for k, v in kw.items():
        s += ' %s=%s' % (k, v)
    return s


def wait_play():
    # after a goal: the celebration and the kick-off (A skips what can be skipped)
    return ['if gs != 5', 'mash 100 30 gs == 5 3000', 'endif']


def W(cond, t):
    # a wait whose timeout is logged and does not end the run (harness `tap` with no button)
    return 'tap 0 1 %s %d' % (cond, t)


# open play with one of pad 0's FIELDERS controlled: task state 2 (not a Mega Strike / goal NIS) and ctl >= 0; when the keeper holds the
# ball the pad controls him (ctl -1) and A passes it out (tapped every 20 frames; a timeout is logged, not fatal)
READY = 'tap 100 20 task == 2 && ctl >= 0 900'


def use_item(slot_type, frames=150, captain=False):
    """press X until the front slot no longer holds slot_type (the item left the slot), then let it act"""
    L = [READY, 'tap 400 12 item0 != %s 240' % slot_type]
    if captain:
        # Bowser, Wario, Waluigi, Petey: X again turns the aiming tank on, X held aims, release fires; the others ignore it
        L += ['idle 20', 'aim opp 400 dopp < 2 60', 'idle 30', 'aim opp 400 dopp < 2 60', 'idle 30', 'aim opp 400 dopp < 2 60']
    L += ['idle %d' % frames]
    return L


def item_phases(name, give, slot_type, count, captain=False):
    L = []
    # 1. open play, at an opponent
    L += wait_play()
    L += ['log ITEM %s phase open' % name, READY, 'item me %s %d set' % (give, count), W('item0 == %s' % slot_type, 120),
          'aim opp 0 dopp < 4.5 240', 'log ITEM %s use open' % name]
    L += use_item(slot_type, 180, captain)
    # 2. near the goal, at the keeper
    L += wait_play()
    L += ['log ITEM %s phase goal' % name, READY, 'aim goal 0 dgoal < 13 420', 'item me %s %d set' % (give, count), W('item0 == %s' % slot_type, 120),
          'aim keeper 0 dkeep < 9 60', 'log ITEM %s use keeper' % name, 'aim keeper 400 item0 != %s 30' % slot_type]
    L += use_item(slot_type, 180, captain)
    # 3. the cycle: a mushroom goes in behind, Z brings it to the front, X uses it, then X uses the item
    L += wait_play()
    L += ['log ITEM %s phase cycle' % name, READY, 'item me %s %d set' % (give, count), 'item me mushroom 1 add', W('item0b == 7', 120),
          'log ITEM %s cycle press Z' % name, 'press 10 3 10', W('item0 == 7', 60), 'log ITEM %s cycled: mushroom in front' % name,
          READY, 'tap 400 12 item0 != 7 240', 'idle 90', W('item0 == %s' % slot_type, 60), 'aim opp 0 dopp < 4.5 240', 'log ITEM %s use after cycle' % name]
    L += use_item(slot_type, 180, captain)
    return L


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'scripts'))
    ap.add_argument('--frames', type=int, default=9000)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    runs = []

    def emit(name, what, lines, frames=None):
        path = os.path.join(a.out, name + '.txt')
        with open(path, 'w') as f:
            f.write('\n'.join(lines) + '\n')
        runs.append((name, path, frames or a.frames))

    k = 0
    for it in ITEMS:
        for cnt in [1] + MULTI.get(it, []):
            if it in ('chomp', 'mushroom', 'star') and cnt != 1:
                continue
            st = [13, 0, 2, 3, 5, 8, 10, 12, 14, 1][k % 10]
            home, away = 0, [6, 1, 7, 9, 11, 3, 5][k % 7]
            k += 1
            name = 'it_%s' % it + ('' if cnt == 1 else '_%d' % cnt)
            lines = PRE.format(what='item %s x%d' % (it, cnt), launch=launch(st, home, away)).splitlines()
            lines += item_phases(name, it, ITEM_ID[it], cnt)
            lines += POST.splitlines()
            emit(name, it, lines)
    for i, it in enumerate(['green', 'red', 'spiny', 'freeze', 'banana', 'bobomb']):
        st = [6, 7, 9, 11, 4, 15][i]
        name = 'gi_%s' % it
        lines = PRE.format(what='giant %s (GIANT cheat)' % it, launch=launch(st, 4, [1, 7, 6, 8, 3, 9][i], powerup=4)).splitlines()
        lines += item_phases(name, it, ITEM_ID[it], 1)
        lines += POST.splitlines()
        emit(name, it, lines)
    for c, cap in enumerate(CAPTAINS):
        st = [13, 1, 10, 6, 2, 12, 9, 4, 5, 7, 3, 14][c]
        away = (c + 5) % 12
        name = 'cap_%s' % cap
        lines = PRE.format(what='captain Super Ability: %s' % cap, launch=launch(st, c, away)).splitlines()
        # the captain item's type is the captain's own (9..20): the script reads it from the slot after the give
        typ = {'mario': 9, 'peach': 10, 'donkeykong': 11, 'bowser': 12, 'luigi': 13, 'daisy': 14, 'wario': 15, 'waluigi': 16, 'bowserjr': 17,
               'diddykong': 18, 'yoshi': 19, 'petey': 20}[cap]
        lines += item_phases(name, 'captain', typ, 1, captain=True)
        lines += POST.splitlines()
        emit(name, cap, lines, 10000)
    # natural acquisition: no hook. Pad 0 hunts the ball carrier and hits (Y) / slides (B) into him: fouls give the OTHER team items;
    # the AI's hits on pad 0's team give it items, which pad 0 uses (X) and cycles (Z) as they come
    for n, (st, home, away) in enumerate([(13, 0, 6), (8, 5, 1), (2, 7, 11)]):
        name = 'nat_%d' % n
        lines = PRE.format(what='natural acquisition (hits and slides on the ball carrier)', launch=launch(st, home, away, skill=2)).splitlines()
        for r in range(40):
            lines += wait_play()
            lines += ['aim owner 0 downer < 2.5 150', 'press %s 3 6' % ('800' if r % 2 == 0 else '200'), 'idle 20']
            lines += ['if item0 >= 0 && ctl >= 0', 'log ITEM nat use', 'press 400 3 20', 'endif']
            if r % 5 == 4:
                lines += ['if item0b >= 0', 'log ITEM nat cycle', 'press 10 3 10', 'endif']
        lines += POST.splitlines()
        emit(name, 'natural', lines, 12000)
    # the INFINITE cheat: both teams get an item every 7.5 s; pad 0 uses them at the nearest opponent and cycles
    for n, (st, home, away) in enumerate([(5, 1, 8), (11, 9, 2)]):
        name = 'inf_%d' % n
        lines = PRE.format(what='INFINITE cheat (periodic items)', launch=launch(st, home, away, powerup=10, skill=2)).splitlines()
        for r in range(24):
            lines += wait_play()
            lines += ['aim opp 0 dopp < 4 150', 'if item0b >= 0', 'press 10 3 10', 'endif', 'if item0 >= 0 && ctl >= 0', 'log ITEM inf use', 'press 400 3 20', 'endif', 'idle 120']
        lines += POST.splitlines()
        emit(name, 'infinite', lines, 12000)
    # replays and celebrations: the item's effect on a character that is drawn from the CPU-skinned path (GPU-skinned with MSC_SKIN_GPU=1):
    # use the item, then take the ball to the goal and shoot (B), so that the goal's celebration and replay show the effect
    for name, home, give, typ in (('rp_mario', 0, 'captain', 9), ('rp_bowserjr', 9, 'captain', 17), ('rp_freeze', 5, 'freeze', 3),
                                  ('rp_luigi', 4, 'captain', 13)):
        lines = PRE.format(what='replay / celebration with %s' % give, launch=launch(13, home, 6)).splitlines()
        for k in range(3):
            lines += wait_play()
            lines += ['log ITEM %s phase replay%d' % (name, k), READY, 'item me %s 1 set' % give, W('item0 == %d' % typ, 120),
                      'aim opp 0 dopp < 5 200', 'log ITEM %s use replay%d' % (name, k), 'tap 400 12 item0 != %d 240' % typ, 'idle 20',
                      'aim ball 0 own == 0 400', 'aim goal 0 dgoal < 11 400', 'hold 24 200 0 0', 'idle 10', W('score0 > %d' % k, 240),
                      W('task == 2', 2400)]
        lines += POST.splitlines()
        emit(name, 'replay', lines, 12000)
    with open(os.path.join(a.out, 'runs.txt'), 'w') as f:
        for r in runs:
            f.write('%s %s %d\n' % r)
    print('%d scripts in %s' % (len(runs), a.out))


if __name__ == '__main__':
    main()
