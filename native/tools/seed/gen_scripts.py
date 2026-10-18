#!/usr/bin/env python3
"""W5-SEED: generates the nav scripts of the shader-key sweeps (tools/seed/scripts/<set>/<name>.txt) and the list of runs.

Each script is one host32 run (tools/seed/rec.sh): boot to the title, then one scenario, then `exit`.
A scenario starts a match or pushes a front-end scene directly (harness commands `launch` / `scene`), so no menu has to be walked
except where the walk itself is the scenario (the Domination flow, the main menu items).

  gen_scripts.py [--out DIR] [--frames N]       writes the scripts and DIR/runs.txt  (name script maxframes timeout)

Sets (names prefix the scripts):
  ai_<stadium>    friendly match, nobody plays (AI against AI, all overlays), the whole match + full-time + post-game
  hu_<stadium>    friendly match, pad 0 plays home with a random-input "monkey" (seeded: deterministic)
  dm_<stadium>    demo match (the title's attract mode: AI against AI, demo overlay)
  cap_<n>         one captain as home and another as away, sidekicks rotating, in a rotating stadium; walk-out cut-scene NOT skipped
  cheat_<kind>_<n> powerup / environment / player cheats (friendly mode)
  sc_<id>         one front-end scene pushed from the title, idle, (own process: a scene that needs state may crash)
  menu_*          real menu walks
"""
import argparse, os, random

STADIUMS = ['battledome', 'bowserstadium', 'craterfield', 'crystalcanyon', 'dump', 'galacticstadium', 'kongacoliseum', 'lavapit', 'pipelinecentral',
            'stormship', 'thepalace', 'thunderisland', 'underground', 'vice', 'wastelands', 'sandtomb', 'tutorialfield']   # ids 0..16 (17 = low poly, unused)
CAPTAINS = ['mario', 'bowser', 'daisy', 'donkeykong', 'luigi', 'peach', 'waluigi', 'wario', 'yoshi', 'bowserjr', 'diddykong', 'petey']
SIDEKICKS = ['toad', 'koopa', 'hammerbros', 'birdo', 'boo', 'drybones', 'montymole', 'shyguy']
POWERUPS = ['none', 'explosives', 'freezing', 'shells', 'giant', 'accelerator', 'peelinout', 'heatseekers', 'bombsaway', 'super', 'infinite', 'butterfingers']
ENVS = ['none', 'secure', 'power', 'voltage', 'tilt', 'whiteball']
PLAYERS = ['none', 'devastating', 'safe', 'skillshot', 'glassjaw']
PRE = """# W5-SEED generated script: {what}
# A dismisses the wrist-strap screen and skips the intro movie; the title follows.
mash 100 20 scene == 0
log STATE title screen
idle {pre}
"""
POST = """log STATE script end
exit
"""
# the front-end scene ids worth visiting (include/Game/BaseGameSceneManager.h); the match / loading / online-only ones are covered elsewhere
SCENES = [1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 13, 14, 15, 16, 20, 21, 22, 23, 24, 27, 28, 29, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48,
          49, 50, 51, 52, 53, 54, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87]


def launch(mode, stadium, home, away, hsk, ask, **kw):
    s = 'launch mode=%s stadium=%d home=%d away=%d hsk=%s ask=%s' % (mode, stadium, home, away, ','.join(map(str, hsk)), ','.join(map(str, ask)))
    for k, v in kw.items():
        s += ' %s=%s' % (k, v)
    return s


def monkey(seed, frames, buttons=(0x100, 0x200, 0x400, 0x800, 0x10, 0x20, 0x40), bias_goal=True):
    """Random held stick directions and buttons (like tools/nav/nav_fulltime.txt's hold lines, but seeded): `frames` presented frames in all."""
    r = random.Random(seed)
    out = []
    n = 0
    while n < frames:
        d = r.choice([4, 6, 8, 10, 14, 20, 30])
        d = min(d, frames - n)
        ang = r.random() * 6.2831853
        import math
        sx, sy = int(72 * math.cos(ang)), int(72 * math.sin(ang))
        if bias_goal and r.random() < 0.35:
            sx, sy = (72, 0) if r.random() < 0.5 else (-72, 0)
        b = 0
        if r.random() < 0.55:
            b = r.choice(buttons)
            if r.random() < 0.12:
                b |= r.choice(buttons)
        out.append('hold %d %x %d %d' % (d, b, sx, sy))
        n += d
    return out


def ring_select(ctx, steps=12):
    """The main menu is a ring: the pointer goes to the button in the direction of the stick (an absolute mapping, found with diag_menu.txt: eight stick
    directions reached seven different buttons). Without knowing the mapping: hold the stick in `steps` directions in turn, each only while the pointer is not
    yet on <ctx> (script variable onctx); the pointer stays on the button when the stick is released."""
    import math
    L = []
    for i in range(steps):
        ang = 2 * math.pi * i / steps
        L += ['if onctx != %d' % ctx, 'hold 12 0 %d %d' % (round(72 * math.cos(ang)), round(72 * math.sin(ang))), 'idle 24', 'endif']
    return L


def match_flow(skip_walkout, play_lines, post_game=True):
    L = ['wait ingame == 1 4000', 'log STATE match object']
    if skip_walkout:
        L += ['mash 100 30 gs == 1 4000', 'log STATE kick-off']
    L += ['wait gs == 5 6000', 'log STATE open play']
    L += play_lines
    return L


FULLTIME_TAIL = """# the match is over: full time banner, post-game screen, results (as tools/nav/nav_fulltime.txt)
wait gs == 3 || gs == 6 || gs == 4 || ingame == 0 3000
log STATE full time (gs 3 / 6)
idle 120
press 100
idle 200
"""


# the post-game flow of tools/nav/nav_fulltime.txt (NPB2): A brings up the post-game overlay (91, "striker_times_2": an article with a scroll bar); its DONE button
# only appears once the article has been scrolled to the end (pointer on the down arrow: DOWN, then A over and over); DONE is left of the arrow: LEFT, A;
# DONE ends the results and the default series starts the rematch (state 3 -> 0 -> 1 -> 5).
POSTGAME = ['wait gs == 3 || gs == 4 || ingame == 0 26000', 'log STATE full time (gs 3 or 4; overtime is gs 6)', 'mash 100 40 ovl == 91 3000', 'wait nbuttons >= 2', 'idle 200',
            'log STATE post-game screen: scrolling the article down', 'press 4', 'idle 20', 'mash 100 8 nbuttons >= 3 2400', 'log STATE post-game: DONE button visible', 'idle 60',
            'press 1', 'idle 30', 'press 100', 'wait gs == 0 || gs == 1 || gs == 5 2000', 'log STATE results done, rematch started', 'idle 300']


def write(outdir, name, what, body, pre=30):
    os.makedirs(outdir, exist_ok=True)
    p = os.path.join(outdir, name + '.txt')
    with open(p, 'w') as f:
        f.write(PRE.format(what=what, pre=pre) if pre is not None else '# W5-SEED generated script: %s\n' % what)
        f.write('\n'.join(body) + '\n')
        f.write(POST)
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'scripts'))
    ap.add_argument('--frames', type=int, default=3600, help='frames of play per scenario (the whole 3-minute match is 10800)')
    ap.add_argument('--scenes2', action='store_true', help='write ONLY the front-end scene sets with a game state set first (`launch ... push=0`): sc2_<id> friendly, sc3_<id> cup, sc4_<id> '
                    'challenge mode; a scene that needs teams / a mode crashes when pushed from the bare title (sc_<id>)')
    ap.add_argument('--pause', action='store_true', help='write ONLY the pause-menu set pause_<n>: START during open play, then DOWN / A / B through the pause items and the screens behind them')
    ap.add_argument('--menus', action='store_true', help='write ONLY the main-menu walks mn_<item>: the ring menu pointer is steered with ring_select, A opens the item and walks on')
    ap.add_argument('--modes', action='store_true', help='write ONLY mt_challenge_<n>: a match in the Striker Challenge mode (the challenge preview overlay is dismissed with A)')
    ap.add_argument('--cover', action='store_true', help='write ONLY the cross-coverage set cv_<stadium>_<k>: in EVERY stadium every captain (6 matches of 2) and every sidekick appears (a long match showed 10 new fragment shaders '
                    'for one captain pair in a fogged stadium: a character material meets the stadium state); AI against AI, the walk-out not skipped')
    ap.add_argument('--cheatcross', action='store_true', help='write ONLY cvp_*: every powerup (11) in 4 stadiums, every environment cheat (5) and player cheat (4) in 3 stadiums: an effect material meets the stadium state (fog / lighting) '
                    'as a character does (cv_); AI against AI, the walk-out not skipped')
    ap.add_argument('--weather', action='store_true', help='write ONLY the forced-weather set wx_<stadium> (17): `setf` makes lightning, wind (rare by chance) and the timed weather start at once')
    ap.add_argument('--randlong', type=int, default=0, metavar='N', help='write ONLY N random WHOLE 3-minute matches lgr_<k>_<stadium> (random captains / sidekicks / cheats, half with a monkey on pad 0, '
                    'half of them with the weather start chances forced): the long tail of rare events (super strikes, items, weather, replays)')
    ap.add_argument('--random', type=int, default=0, metavar='N', help='write ONLY N random scenarios rnd_<k>_<stadium> (stadium, captains, sidekicks, cheats, human or AI, input seed all drawn from --seed): '
                    'more samples of the same kind of scenario; the saturation of NEW shaders over them is the evidence that the catalogue is complete')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--prefix', default='rnd', help='name prefix of the --random set (hx = the final held-out set, kept out of the union)')
    ap.add_argument('--postgame', action='store_true', help='write ONLY the short full-match set (ft2_*) with the working post-game flow')
    ap.add_argument('--long', action='store_true', help='write ONLY the long set lg_<n>_<stadium>: a whole 3-minute match per stadium, AI against AI (weather and hazards start by chance, '
                    'so a long match is the only way to meet them), through full time, the post-game screens and the results')
    ap.add_argument('--heldout', action='store_true', help='write ONLY the held-out set ho_<n>_<stadium>: random captains / sidekicks / cheats / input per stadium, run AFTER the union is made '
                    'to measure how many keys a fresh scenario still adds')
    a = ap.parse_args()
    runs = []

    def add(name, what, body, maxf, tmo, pre=30):
        if a.postgame and not name.startswith('ft2_'):
            return
        if a.menus and not name.startswith('mn_'):
            return
        p = write(a.out, name, what, body, pre)
        runs.append((name, os.path.relpath(p, os.path.dirname(os.path.abspath(__file__)) + '/../..'), maxf, tmo))

    if a.cheatcross:
        n = 0
        for kind, names, count, per in (('powerup', POWERUPS, 11, 4), ('env', ENVS, 5, 3), ('player', PLAYERS, 4, 3)):
            for k in range(1, count + 1):
                for j in range(per):
                    i = (k * 3 + j * 5 + (1 if kind == 'env' else 2 if kind == 'player' else 0)) % 17
                    h, aw = (k * 5 + j * 3) % 12, (k * 5 + j * 3 + 7) % 12
                    body = [launch('friendly', i, h, aw, [(k + j) % 8, (k + j + 1) % 8, (k + j + 2) % 8], [(k + j + 3) % 8, (k + j + 4) % 8, (k + j + 5) % 8], time=180, **{kind: k})] + \
                           match_flow(False, ['idle 1200'], False)
                    add('cvp_%s%02d_%02d_%s' % (kind, k, i, STADIUMS[i]), '%s cheat %s in %s' % (kind, names[k], STADIUMS[i]), body, 10000, 120)
                    n += 1
        os.makedirs(a.out, exist_ok=True)
        with open(os.path.join(a.out, 'runs.txt'), 'w') as f:
            for r in runs:
                f.write('%s %s %d %d\n' % r)
        print('%d cheat-cross scripts in %s' % (len(runs), a.out))
        return
    if a.cover:
        for i, st in enumerate(STADIUMS):
            order = [(c + i) % 12 for c in range(12)]
            for k in range(6):
                h, aw = order[2 * k], order[2 * k + 1]
                hs = [(k * 6 + j + i) % 8 for j in range(3)]
                asw = [(k * 6 + 3 + j + i) % 8 for j in range(3)]
                body = [launch('friendly', i, h, aw, hs, asw, time=180)] + match_flow(False, ['idle 900'], False)
                add('cv_%02d_%s_%d' % (i, st, k), 'coverage: %s v %s in %s' % (CAPTAINS[h], CAPTAINS[aw], st), body, 9000, 120)
        os.makedirs(a.out, exist_ok=True)
        with open(os.path.join(a.out, 'runs.txt'), 'w') as f:
            for r in runs:
                f.write('%s %s %d %d\n' % r)
        print('%d coverage scripts in %s' % (len(runs), a.out))
        return
    if a.modes:
        for k, st in enumerate((16, 13, 6, 9)):
            body = [launch('challenge', st, k % 12, (k + 4) % 12, [0, 1, 2], [3, 4, 5], human=0, time=120), 'wait ingame == 1 4000', 'log STATE match object',
                    'mash 100 40 gs == 5 4000', 'log STATE open play'] + monkey(9500 + k, 2400)
            add('mt_challenge_%02d_%s' % (st, STADIUMS[st]), 'Striker Challenge mode match in %s' % STADIUMS[st], body, 14000, 200)
        os.makedirs(a.out, exist_ok=True)
        with open(os.path.join(a.out, 'runs.txt'), 'w') as f:
            for r in runs:
                f.write('%s %s %d %d\n' % r)
        print('%d mode scripts in %s' % (len(runs), a.out))
        return
    if a.weather:
        for i, st in enumerate(STADIUMS):
            body = ['setf gLightningStartChance 100', 'setf gWindStartChance 100', 'setf gLavaStartChance 100', 'setf gStormStartDelay 1',
                    launch('friendly', i, (i * 5 + 2) % 12, (i * 5 + 8) % 12, [i % 8, (i + 1) % 8, (i + 2) % 8], [(i + 3) % 8, (i + 4) % 8, (i + 5) % 8], human=0, time=120)] + \
                   match_flow(True, monkey(9000 + i, 5400))
            add('wx_%02d_%s' % (i, st), 'forced weather in %s (lightning / wind / lava / storm start chances at 100%%)' % st, body, 14000, 200)
        os.makedirs(a.out, exist_ok=True)
        with open(os.path.join(a.out, 'runs.txt'), 'w') as f:
            for r in runs:
                f.write('%s %s %d %d\n' % r)
        print('%d weather scripts in %s' % (len(runs), a.out))
        return
    if a.pause:
        for k, st in enumerate((5, 11, 2)):
            walk = []
            for j in range(7):
                walk += ['press 4', 'idle 25', 'log STATE pause item %d' % j, 'press 100', 'idle 120', 'press 200', 'idle 60']
            body = [launch('friendly', st, (k * 3 + 1) % 12, (k * 3 + 7) % 12, [k, k + 1, k + 2], [k + 3, k + 4, k + 5], human=0, time=120)] + match_flow(True, monkey(8000 + k, 400)) + \
                   ['press 1000', 'idle 90', 'log STATE paused'] + walk + ['press 200', 'idle 60', 'press 1000', 'idle 120'] + monkey(8100 + k, 600)
            add('pause_%02d_%s' % (k, STADIUMS[st]), 'pause menu walk in %s' % STADIUMS[st], body, 20000, 200)
        os.makedirs(a.out, exist_ok=True)
        with open(os.path.join(a.out, 'runs.txt'), 'w') as f:
            for r in runs:
                f.write('%s %s %d %d\n' % r)
        print('%d pause scripts in %s' % (len(runs), a.out))
        return
    if a.scenes2:
        for pfx, mode, ids in (('sc2', 'friendly', SCENES), ('sc3', 'cup', SCENES), ('sc4', 'challenge', [x for x in SCENES if x in (31, 32, 33, 34, 35, 36, 37, 38, 39, 58, 59, 60, 61, 75, 76, 77, 84, 85, 86, 87)])):
            for sid in ids:
                body = [launch(mode, 5, 0, 3, [0, 1, 2], [3, 4, 5], push=0), 'scene %d' % sid, 'idle 240', 'popscene', 'idle 60']
                add('%s_%02d' % (pfx, sid), 'front-end scene %d pushed from the title after launch mode=%s push=0' % (sid, mode), body, 4000, 60)
        os.makedirs(a.out, exist_ok=True)
        with open(os.path.join(a.out, 'runs.txt'), 'w') as f:
            for r in runs:
                f.write('%s %s %d %d\n' % r)
        print('%d scene scripts in %s' % (len(runs), a.out))
        return
    if a.randlong:
        rr = random.Random(a.seed * 104729)
        for k in range(a.randlong):
            i = rr.randrange(17)
            st = STADIUMS[i]
            h, aw = rr.sample(range(12), 2)
            hs = [rr.randrange(8) for _ in range(3)]
            asw = [rr.randrange(8) for _ in range(3)]
            kw = {}
            if rr.random() < 0.5:
                kw['powerup'] = rr.randrange(1, 12)
            if rr.random() < 0.2:
                kw['env'] = rr.randrange(1, 6)
            if rr.random() < 0.2:
                kw['player'] = rr.randrange(1, 5)
            human = rr.random() < 0.5
            forced = rr.random() < 0.5
            pre = ['setf gLightningStartChance 100', 'setf gWindStartChance 100', 'setf gStormStartDelay 1'] if forced else []
            if human:
                kw['human'] = 0
            tail = ['wait gs == 3 || gs == 4 || ingame == 0 26000', 'log STATE full time', 'idle 400']
            play = monkey(a.seed * 1000 + k, 10600) if human else ['idle 10000']
            body = pre + [launch('friendly', i, h, aw, hs, asw, time=180, **kw)] + match_flow(False, [], False)
            # a monkey plays through the match while the wait for the end of it runs: the script commands run one after the other, so the monkey's frames ARE the match
            body += (play if human else []) + tail
            add('lgr_%03d_%s' % (k, st), 'random whole match: %s v %s in %s %s%s' % (CAPTAINS[h], CAPTAINS[aw], st, kw, ' weather forced' if forced else ''), body, 36000, 240)
        os.makedirs(a.out, exist_ok=True)
        with open(os.path.join(a.out, 'runs.txt'), 'w') as f:
            for r in runs:
                f.write('%s %s %d %d\n' % r)
        print('%d random long scripts in %s' % (len(runs), a.out))
        return
    if a.random:
        rr = random.Random(a.seed * 7919)
        for k in range(a.random):
            i = rr.randrange(17)
            st = STADIUMS[i]
            h, aw = rr.sample(range(12), 2)
            hs = [rr.randrange(8) for _ in range(3)]
            asw = [rr.randrange(8) for _ in range(3)]
            kw = {}
            if rr.random() < 0.35:
                kw['powerup'] = rr.randrange(12)
            if rr.random() < 0.15:
                kw['env'] = rr.randrange(6)
            if rr.random() < 0.15:
                kw['player'] = rr.randrange(5)
            human = rr.random() < 0.7
            if human:
                body = [launch('friendly', i, h, aw, hs, asw, human=0, time=120, **kw)] + match_flow(False, monkey(a.seed * 100000 + k, a.frames))
            else:
                body = [launch('friendly', i, h, aw, hs, asw, time=120, **kw)] + match_flow(False, ['idle %d' % a.frames], False)
            add('%s_%03d_%s' % (a.prefix, k, st), 'random: %s v %s in %s %s %s' % (CAPTAINS[h], CAPTAINS[aw], st, kw, 'human' if human else 'AI'), body, a.frames + 8000, 200)
        os.makedirs(a.out, exist_ok=True)
        with open(os.path.join(a.out, 'runs.txt'), 'w') as f:
            for r in runs:
                f.write('%s %s %d %d\n' % r)
        print('%d random scripts in %s' % (len(runs), a.out))
        return
    if a.long:
        for i, st in enumerate(STADIUMS):
            h, aw = (i * 7 + 2) % 12, (i * 7 + 5) % 12
            hs = [(i * 3 + k) % 8 for k in range(3)]
            asw = [(i * 3 + 4 + k) % 8 for k in range(3)]
            body = [launch('friendly', i, h, aw, hs, asw, time=180)] + match_flow(False, POSTGAME, False)
            add('lg_%02d_%s' % (i, st), 'whole 3-minute match in %s (%s v %s), AI against AI' % (st, CAPTAINS[h], CAPTAINS[aw]), body, 36000, 240)
        os.makedirs(a.out, exist_ok=True)
        with open(os.path.join(a.out, 'runs.txt'), 'w') as f:
            for r in runs:
                f.write('%s %s %d %d\n' % r)
        print('%d long scripts in %s' % (len(runs), a.out))
        return
    if a.heldout:
        rr = random.Random(424242)
        for i, st in enumerate(STADIUMS):
            h, aw = rr.sample(range(12), 2)
            hs = [rr.randrange(8) for _ in range(3)]
            asw = [rr.randrange(8) for _ in range(3)]
            kw = {}
            if rr.random() < 0.4:
                kw['powerup'] = rr.randrange(12)
            if rr.random() < 0.2:
                kw['env'] = rr.randrange(6)
            if rr.random() < 0.2:
                kw['player'] = rr.randrange(5)
            body = [launch('friendly', i, h, aw, hs, asw, human=0, time=120, **kw)] + match_flow(False, monkey(20000 + i, a.frames))
            add('ho_%02d_%s' % (i, st), 'held-out: %s v %s in %s %s' % (CAPTAINS[h], CAPTAINS[aw], st, kw), body, a.frames + 8000, 200)
        os.makedirs(a.out, exist_ok=True)
        with open(os.path.join(a.out, 'runs.txt'), 'w') as f:
            for r in runs:
                f.write('%s %s %d %d\n' % r)
        print('%d held-out scripts in %s' % (len(runs), a.out))
        return

    # ---- stadium sweeps
    for i, st in enumerate(STADIUMS):
        h, aw = (i * 5 + 1) % 12, (i * 5 + 7) % 12
        if h == aw: aw = (aw + 1) % 12
        hs = [(i + k) % 8 for k in range(3)]
        asw = [(i + 3 + k) % 8 for k in range(3)]
        # AI against AI, friendly mode (HUD, goal overlay, mega strike meter, pip, super ability): the walk-out plays, replays are not skipped
        body = [launch('friendly', i, h, aw, hs, asw, time=120)] + match_flow(False, ['idle %d' % a.frames], False)
        add('ai_%02d_%s' % (i, st), 'AI against AI in %s (%s v %s)' % (st, CAPTAINS[h], CAPTAINS[aw]), body, a.frames + 6000, 200)
        # pad 0 plays home with random input
        body = [launch('friendly', i, aw, h, asw, hs, human=0, time=120)] + match_flow(True, monkey(1000 + i, a.frames))
        add('hu_%02d_%s' % (i, st), 'human (monkey) in %s (%s v %s)' % (st, CAPTAINS[aw], CAPTAINS[h]), body, a.frames + 6000, 200)
    # the title's attract-mode demo, a few stadiums
    for i in (0, 5, 13):
        body = [launch('demo', i, (i + 2) % 12, (i + 9) % 12, [0, 1, 2], [3, 4, 5])] + match_flow(False, ['idle %d' % a.frames], False)
        add('dm_%02d_%s' % (i, STADIUMS[i]), 'demo match in %s' % STADIUMS[i], body, a.frames + 6000, 200)

    # ---- captains / sidekicks: every captain as home (human, walk-out not skipped) and as away; stadiums rotate
    for c in range(12):
        away = (c + 5) % 12
        st = (c * 3 + 2) % 17
        hs = [(c + k) % 8 for k in range(3)]
        asw = [(c + 4 + k) % 8 for k in range(3)]
        body = [launch('friendly', st, c, away, hs, asw, human=0, time=120)] + match_flow(False, monkey(2000 + c, a.frames))
        add('cap_%02d_%s' % (c, CAPTAINS[c]), '%s (home, human) v %s in %s' % (CAPTAINS[c], CAPTAINS[away], STADIUMS[st]), body, a.frames + 8000, 200)

    # ---- whole short matches (25 s of game time): full time, the post-game screens, the results, the rematch
    def ft_set():
        for k, st in enumerate((0, 8, 13, 5)):
            body = [launch('friendly', st, (k * 4 + 3) % 12, (k * 4 + 9) % 12, [k, k + 1, k + 2], [k + 3, k + 4, k + 5], human=0, time=25)] + match_flow(True, monkey(7000 + k, 1500)) + POSTGAME
            add(('ft2_' if a.postgame else 'ft_') + '%02d_%s' % (k, STADIUMS[st]), 'whole 25 s match in %s: full time, post-game, results' % STADIUMS[st], body, 14000, 200)
    if a.postgame:
        ft_set()
        os.makedirs(a.out, exist_ok=True)
        with open(os.path.join(a.out, 'runs.txt'), 'w') as f:
            for r in runs:
                f.write('%s %s %d %d\n' % r)
        print('%d post-game scripts in %s' % (len(runs), a.out))
        return
    ft_set()

    # ---- cheats (friendly): powerups, environment, player
    for k in range(1, 12):
        st = (k * 2) % 17
        body = [launch('friendly', st, k % 12, (k + 3) % 12, [k % 8, (k + 1) % 8, (k + 2) % 8], [(k + 4) % 8, (k + 5) % 8, (k + 6) % 8], human=0, powerup=k, time=120)] + \
               match_flow(True, monkey(3000 + k, a.frames // 2))
        add('cheat_pu%02d_%s' % (k, POWERUPS[k]), 'powerup cheat %s' % POWERUPS[k], body, a.frames + 6000, 200)
    for k in range(1, 6):
        st = (k * 3 + 1) % 17
        body = [launch('friendly', st, (k + 1) % 12, (k + 6) % 12, [0, 1, 2], [3, 4, 5], human=0, env=k, time=120)] + match_flow(True, monkey(4000 + k, a.frames // 2))
        add('cheat_env%d_%s' % (k, ENVS[k]), 'environment cheat %s' % ENVS[k], body, a.frames + 6000, 200)
    for k in range(1, 5):
        st = (k * 4) % 17
        body = [launch('friendly', st, (k + 2) % 12, (k + 7) % 12, [0, 1, 2], [3, 4, 5], human=0, player=k, time=120)] + match_flow(True, monkey(5000 + k, a.frames // 2))
        add('cheat_pl%d_%s' % (k, PLAYERS[k]), 'player cheat %s' % PLAYERS[k], body, a.frames + 6000, 200)

    # ---- front-end scenes, one process each
    for sid in SCENES:
        body = ['scene %d' % sid, 'idle 240', 'popscene', 'idle 60']
        add('sc_%02d' % sid, 'front-end scene %d pushed from the title' % sid, body, 4000, 60)

    # ---- real menu walks that need game state: the main menu items Road to the Striker Cup (2), Striker Challenges (3), Strikers 101 (4)
    # A is pressed over and over until the match exists (the default button of every screen and pop-up on the way leads on).
    MENU = """mash 100 30 scene != 0
wait scene == 1
wait scene == 10 || nbuttons >= 8
if scene == 10
log first run: no save file, accepting 'create save data'
wait nbuttons >= 2
idle 10
mash 100 30 scene == 1
wait nbuttons >= 8
endif
wait onctx == 0
log STATE main menu, pointer on item 0
idle 30
"""
    for ctx, nm in ((2, 'cup'), (3, 'challenge'), (4, 'strikers101'), (1, 'online'), (5, 'halloffame'), (6, 'options')):
        body = MENU.split('\n') + ring_select(ctx) + ['idle 20', 'log STATE main menu item %d under the pointer' % ctx, 'press 100', 'idle 300']
        if ctx in (2, 3, 4):
            body += ['mash 100 45 ingame == 1 12000', 'log STATE match object', 'mash 100 30 gs == 5 8000', 'log STATE open play'] + monkey(6000 + ctx, a.frames // 2)
        else:
            # a screen with items: down, A (open), wait, B (back), over and over
            for j in range(8):
                body += ['press 4', 'idle 25', 'log STATE explore item %d' % j, 'press 100', 'idle 100', 'press 200', 'idle 60']
            body += ['press 200', 'idle 60']
        add('mn_%s' % nm, 'main menu item %d (%s): A until the next state' % (ctx, nm), body, 40000, 200)
    # intro movie played to its end; boot to title
    add('movie_intro', 'the intro movie plays to its end by itself', ['wait scene == 22 3000', 'wait scene == 0 20000', 'idle 600'], 20000, 100, None)

    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, 'runs.txt'), 'w') as f:
        for r in runs:
            f.write('%s %s %d %d\n' % r)
    print('%d scripts in %s' % (len(runs), a.out))


if __name__ == '__main__':
    main()
