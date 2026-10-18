#!/usr/bin/env python3
"""W5 CTL: generates the Vita-button match scripts (platform/harness: vhold/vpress/vmash/touch/rtouch...; variables c* from
platform/sdk/input/np_game_input.cpp) that drive every action of a match through the layout mapping, not through GameCube bits.

  tools/ctl/gen_scripts.py            -> tools/ctl/nav/ctl_{modern,classic,touch}.txt
  modern   MODERN layout, buttons only (minigames with buttons), rear touch taps for switch / cycle / shakedown
  classic  the same actions with the CLASSIC buttons (run with MSC_CONTROLS=classic)
  touch    MODERN, the minigames with the front touch panel (Mega Save touch, meter finger, shakedown swipes)
The run stops (exit 0) as soon as every counter the variant expects is non-zero, or at the end of the rounds; tools/ctl/check.py
reads the "[ctl] summary" line."""
import os

LAYOUT = {
    'modern': dict(pass_='cross', lob='square', shoot='circle', chip='r+circle', chiphold='r', turbo='r', hit='square', slide='circle',
                   switch='l', item='triangle', cycle='select', mega='circle', meter='circle', save='cross', shake='square', kpass='cross'),
    'classic': dict(pass_='cross', lob='l+cross', shoot='square', chip='l+square', chiphold='l', turbo='l', hit='triangle', slide='square',
                    switch='cross', item='circle', cycle='select', mega='square', meter='square', save='cross', shake='select', kpass='cross'),
}


def minigames(v, touch, rear):
    out = ['if cmode == 1', 'log MINIGAME mega save']
    out += ['try touch 480 300 30 2', 'try touch 520 260 30 2', 'try touch 440 280 30 2'] if touch else ['try vmash %s 8 cmode != 1 600' % v['save']]
    out += ['endif', 'if cmode == 2', 'log MINIGAME our mega strike meter']
    out += ['touch 480 272 10 12', 'touch 480 272 10 12'] if touch else ['try vmash %s 10 cmode != 2 600' % v['meter']]
    out += ['endif', 'if cmode == 3', 'log MINIGAME shakedown']
    if touch:
        out += ['try swmash 300 300 420 300 8 cmode != 3 600']
    elif rear:
        out += ['try rmash 200 272 7 cmode != 3 240', 'try vmash %s 6 cmode != 3 240' % v['shake']]
    else:
        out += ['try vmash %s 6 cmode != 3 600' % v['shake']]
    out += ['endif']
    return out


def attack(v, k):
    # one attack action, rotating with k; the ball carrier is the local player when it runs (vchase: run toward the net)
    acts = [
        ('pass', ['vchase 10 none goal', 'vpress %s 3 20' % v['pass_']]),
        ('deke (right stick)', ['vchase 8 none goal', 'vchase 4 none goal 72 0', 'idle 15']),
        ('lob pass', ['vchase 10 none goal', 'vpress %s 3 25' % v['lob']]),
        ('turbo (chip ahead)', ['vchase 10 none goal', 'vchase 3 %s goal' % v['turbo'], 'vchase 30 none ball']),
        ('deke (D-pad)', ['vchase 8 none goal', 'vhold 3 right', 'idle 15']),
        ('shot', ['vchase 20 none goal', 'vchase 4 %s goal' % v['shoot'], 'idle 30']),
        ('pass', ['vchase 10 none goal', 'vpress %s 3 20' % v['pass_']]),
        ('chip shot', ['vchase 15 none goal', 'vchase 6 %s goal' % v['chip'], 'vchase 4 %s goal' % v['chiphold'], 'idle 30']),
        ('cycle item', ['vpress %s 3 15' % v['cycle']]),
    ]
    name, cmds = acts[k % len(acts)]
    return ['log ACTION %s' % name] + cmds


def defence(v, k, rear):
    acts = [
        ('big hit', ['vchase 10 none ball', 'vpress %s 3 15' % v['hit']]),
        ('switch player', ['vpress %s 3 15' % v['switch']]),
        ('slide tackle', ['vchase 10 none ball', 'vpress %s 3 20' % v['slide']]),
        ('cycle item', ['vpress %s 3 15' % v['cycle']]),
    ]
    if rear:
        acts += [('switch player (rear right tap)', ['rtouch 720 272 3 15']), ('cycle item (rear left tap)', ['rtouch 220 272 3 15'])]
    name, cmds = acts[k % len(acts)]
    return ['log ACTION %s' % name] + cmds


WANT = {
    'modern': ['cpass', 'clob', 'cshot', 'cchip', 'cturbo', 'cdeke', 'chit', 'cslide', 'cswitch', 'ccycle', 'cmeterpress'],
    'classic': ['cpass', 'clob', 'cshot', 'cchip', 'cturbo', 'cdeke', 'chit', 'cslide', 'cswitch', 'ccycle', 'cmeterpress'],
    'touch': ['cpass', 'chit', 'cmega'],
}


def script(variant):
    v = LAYOUT['classic' if variant == 'classic' else 'modern']
    touch = variant == 'touch'
    rear = variant != 'classic'
    want = WANT[variant]
    out = ['# W5 CTL generated (tools/ctl/gen_scripts.py): %s. Vita-button commands only; the layout maps them.' % variant,
           '# Counters: platform/sdk/input/np_game_input.cpp ctl_observe ([ctl] summary at the end); the run stops when every one of',
           '# %s is non-zero and a minigame was played with buttons / touch (or at MSC_MAX_FRAMES).' % ' '.join(want),
           'vmash cross 20 scene == 0', 'log STATE title screen', 'idle 30',
           'launch mode=friendly stadium=0 home=0 away=7 hsk=0,1,2 ask=3,4,5 human=0 skill=2 time=300',
           'wait ingame == 1 4000', 'log STATE match object', 'wait gs == 5 6000', 'log STATE open play',
           # left to right, no precedence: (save blocks || shakes || touch events) && every action
           'stopwhen csavea > 0 || cshake > 0 && ' + ' && '.join('%s > 0' % w for w in want)]
    # one round tries every action once (each guarded by the context it needs), with the minigame handlers in between; the
    # harness repeats it (repeat / endrepeat): a script of a few hundred lines (a long unrolled one does not fit the game's heap)
    body = []
    body += minigames(v, touch, rear)
    # the Mega Strike: the captain holds the shoot button until the meter opens, lets go, then stops the meter (buttons or a finger)
    meter = ['touch 480 272 10 12', 'touch 480 272 10 12'] if touch else ['try vmash %s 12 cmode != 2 300' % v['meter']]
    body += ['if chave == 1 && cplayer == 0 && cmega < 3', 'log ACTION mega strike try (captain holds %s)' % v['mega'],
             'vchase 15 none goal', 'try vmash %s 1 cmode == 2 || chave == 0 200' % v['mega'], 'if cmode == 2',
             'log MINIGAME our mega strike meter (%s)' % ('touch' if touch else v['meter'])] + meter + ['endif', 'endif']
    for k in range(9):
        body += minigames(v, touch, rear)
        body += ['if chave == 1 && cplayer == 4', 'log ACTION keeper pass', 'vpress %s 3 20' % v['kpass'], 'endif']
        body += ['if chave == 1 && cplayer != 4'] + attack(v, k) + ['endif']
        body += ['if chave == 0', 'vchase 12 none ball', 'endif']
        body += ['if cball == 1'] + defence(v, k, rear) + ['endif']
    body += ['if chave == 1 && citems > 0', 'log ACTION use item', 'vpress %s 3 20' % v['item'], 'endif']
    out += ['repeat 400'] + body + ['endrepeat']
    out += ['log STATE script end', 'exit']
    return '\n'.join(out) + '\n'


def touchmeter(finger=True):
    # the Mega Strike meter with a finger: get the ball to the captain (hold switch = the captain; pass otherwise), hold Circle until the
    # meter opens, then a finger on the screen twice; the run stops after two meters were touched (cmeterpress counts finger and button)
    out = ['# W5 CTL generated (tools/ctl/gen_scripts.py): %s. MODERN; the own Mega Strike meter stopped with %s.' %
           (('touchmeter', 'a finger') if finger else ('buttonmeter', 'Circle presses')),
           'vmash cross 20 scene == 0', 'log STATE title screen', 'idle 30',
           'launch mode=friendly stadium=0 home=0 away=7 hsk=0,1,2 ask=3,4,5 human=0 skill=0 time=300',
           'wait ingame == 1 4000', 'wait gs == 5 6000', 'log STATE open play', 'stopwhen cmeterpress >= 2']
    body = ['if chave == 1 && cplayer == 0', 'log ACTION mega strike try (captain holds circle)', 'vchase 10 none goal',
            'try vmash circle 1 cmode == 2 || chave == 0 200', 'if cmode == 2',
            'log MINIGAME our mega strike meter (%s)' % ('touch' if finger else 'circle')]
    body += ['touch 480 272 10 12', 'touch 480 272 10 12'] if finger else ['idle 6', 'try vmash circle 12 cmode != 2 300']
    body += ['endif', 'endif',
            'if chave == 1 && cplayer != 0', 'vchase 8 none goal', 'vpress cross 3 20', 'endif',
            'if chave == 0 && cplayer != 0', 'vhold 18 cross', 'endif',
            'if chave == 0', 'vchase 20 none ball', 'vpress square 3 5', 'endif']
    out += ['repeat 600'] + body + ['endrepeat', 'log STATE script end', 'exit']
    return '\n'.join(out) + '\n'


def lan_match(seed):
    # the match part of a LAN pair (tools/mp/native_pair.py): Vita buttons, sticks and rear taps, the same sequence whatever the layout
    # of the console (the pair runs one console MODERN, the other CLASSIC: the records must agree on both)
    seq = ['vhold 60 none 72 0', 'vpress cross 3 20', 'vhold 40 none 0 72', 'vpress square 3 30', 'vpress circle 12 30',
           'vhold 30 none -60 -40', 'vpress l 3 20', 'vpress r 3 30', 'rtouch 720 272 3 20', 'vpress triangle 3 20',
           'vhold 40 r 72 0', 'vpress r+circle 6 30', 'rtouch 220 272 3 20', 'vpress select 3 20', 'vhold 4 none 0 0 72 0',
           'vhold 30 none 50 50', 'vpress square 3 20', 'vhold 20 l', 'idle 20', 'vhold 3 right', 'idle 20']
    out = []
    for k in range(10):
        out += seq[k % 3:] + seq[:k % 3]
    return out


def lan(role):
    here = os.path.dirname(os.path.abspath(__file__))
    src = os.path.join(here, '..', 'mp', 'lan', 'lan_%s.txt' % role)
    lines = open(src).read().splitlines()
    cut = next(i for i, l in enumerate(lines) if l.strip() == 'log STATE open play') + 1
    head = ['# W5 CTL generated (tools/ctl/gen_scripts.py) from tools/mp/lan/lan_%s.txt: the match part in Vita buttons' % role] + lines[:cut]
    return '\n'.join(head + lan_match(role) + ['log STATE played', 'exit']) + '\n'


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    os.makedirs(os.path.join(here, 'nav'), exist_ok=True)
    for variant in ('modern', 'classic', 'touch'):
        with open(os.path.join(here, 'nav', 'ctl_%s.txt' % variant), 'w') as f:
            f.write(script(variant))
    with open(os.path.join(here, 'nav', 'ctl_touchmeter.txt'), 'w') as f:
        f.write(touchmeter(True))
    with open(os.path.join(here, 'nav', 'ctl_buttonmeter.txt'), 'w') as f:
        f.write(touchmeter(False))
    os.makedirs(os.path.join(here, 'lan'), exist_ok=True)
    for role in ('host', 'join'):
        with open(os.path.join(here, 'lan', 'ctl_lan_%s.txt' % role), 'w') as f:
            f.write(lan(role))


if __name__ == '__main__':
    main()
