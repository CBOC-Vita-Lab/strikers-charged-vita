#!/usr/bin/env python3
"""MSC_VGX_TEV_CANON proof: for every fragment key of a list, the text with the switch off against the text with it on (listtext records, same order),
evaluated by glsl_eval.py over dense random and adversarial fragments: 8-bit outputs and the discard decision compared.
   tev_canon_prove.py <old.txt> <new.txt> <max level difference: 0 for mode 1, 1 for mode 2> [samples] [seed]
Exit 0 only if no fragment of any key differs by more than that. Prints the worst difference per key group and the number of keys proven."""
import os
import re
import sys
import zlib
import numpy as np
sys.path.insert(0, __file__.rsplit('/', 1)[0])
import glsl_eval as G

F = np.float32


def records(path):
    d = open(path).read()
    out = []
    for r in re.split(r'(?m)^(?=#### )', d):
        if r.startswith('####'):
            head, body = r.split('\n', 1)
            out.append((head.split()[1], body))
    return out


def pool(rng, n, wide=False):
    """values k/255 with the corners and the wrap-edge values far more often than uniform: 0, 1, 254/255, 253/255, 128/255, 127/255, 1/255"""
    special = np.array([0, 1, 254, 255, 253, 128, 127, 129, 126, 2, 64, 192, 85, 170], np.float32) / F(255)
    half = (np.arange(0, 256, dtype=np.float32) + F(0.5)) / F(255)            # the rounding edges of the alpha test (k + 0.5) / 255, and the floats either side of them
    edge = np.concatenate([half, np.nextafter(half, F(0)), np.nextafter(half, F(2))]).astype(F)
    edge = edge[(edge >= 0) & (edge <= 1)]
    k = rng.integers(0, 4, n)
    a = special[rng.integers(0, len(special), n)]
    b = (rng.integers(0, 256, n).astype(np.float32)) / F(255)
    c = rng.random(n).astype(np.float32)            # off the 8-bit grid: a texture filter / interpolation gives these
    e = edge[rng.integers(0, len(edge), n)]
    v = np.where(k == 0, np.where(rng.random(n) < 0.35, e, a), np.where(k == 1, b, np.where(k == 2, c, np.where(rng.random(n) < 0.5, F(0), F(1)))))
    if wide:
        w = (rng.integers(-1024, 1024, n).astype(np.float32)) / F(255)   # GXSetTevColorS10: -1024..1023
        v = np.where(rng.random(n) < 0.5, v, w)
    return v.astype(F)


class Inputs:
    def __init__(self, text, n, seed, wide):
        self.n = n
        self.rng = np.random.default_rng(seed)
        m = re.search(r'uniform vec4 u_tev\[(\d+)\]', text)
        nt = int(m.group(1))
        u = []
        for i in range(nt):
            u.append(np.stack([pool(self.rng, n, wide and i < 4) for _ in range(4)], 1))
        # alpha references (BP 0xF3) are whole numbers 0..255 in u_tev[8].xy; constant destination alpha z is 0..1
        if nt > 8:
            sp = np.array([0, 1, 2, 127, 128, 129, 253, 254, 255], np.float32)
            r0 = np.where(self.rng.random(n) < 0.6, sp[self.rng.integers(0, len(sp), n)], self.rng.integers(0, 256, n).astype(np.float32))
            r1 = np.where(self.rng.random(n) < 0.6, sp[self.rng.integers(0, len(sp), n)], self.rng.integers(0, 256, n).astype(np.float32))
            u[8][:, 0] = r0
            u[8][:, 1] = r1
        if nt > 10:     # fog: a / (b - z) - c, colour; plausible values
            u[9][:, 0] = (self.rng.random(n) * 0.5 + 0.01).astype(F)
            u[9][:, 1] = (1.0 + self.rng.random(n)).astype(F)
            u[9][:, 2] = (self.rng.random(n) * 0.3).astype(F)
        self.u = u
        self.cache = {}
        self.var = {}
        for nme in re.findall(r'varying vec4 (v_col\d)', text):
            self.var[nme] = np.stack([pool(self.rng, n) for _ in range(4)], 1)
            ov = float(os.environ.get('VCOL_OVER', '0'))   # an extrapolated vertex colour at a multisampled edge: this far outside 0..1, on half the fragments
            if ov:
                self.var[nme] = np.where(self.rng.random((n, 4)) < 0.5, self.var[nme] + (self.rng.random((n, 4)) * 2 - 1).astype(F) * F(ov), self.var[nme]).astype(F)
        for nme in re.findall(r'varying vec3 (v_tex\d)', text):
            self.var[nme] = np.stack([self.rng.random(n).astype(F), self.rng.random(n).astype(F), (0.5 + self.rng.random(n)).astype(F)], 1)
        for nme in re.findall(r'varying vec2 (v_tex\d)', text):
            self.var[nme] = np.stack([self.rng.random(n).astype(F), self.rng.random(n).astype(F)], 1)
        self.fragz = self.rng.random(n).astype(F)

    def tex(self, m, uvkey):
        key = (m, uvkey)
        if key not in self.cache:
            r = np.random.default_rng(zlib.crc32(repr(key).encode()) ^ 0x5bd1e995)
            self.cache[key] = np.stack([pool(r, self.n) for _ in range(4)], 1)
        return self.cache[key]


def run(text, inp):
    return G.evaluate(text, inp.n, inp.u, inp.tex, inp.var, inp.fragz)


def main():
    old, new = records(sys.argv[1]), records(sys.argv[2])
    tol = int(sys.argv[3])
    n = int(sys.argv[4]) if len(sys.argv) > 4 else 20000
    seed = int(sys.argv[5]) if len(sys.argv) > 5 else 1
    assert len(old) == len(new), (len(old), len(new))
    bad = 0
    same_text = 0
    worst_all = 0
    tot_frag = 0
    tot_diff = 0
    tot_disc = 0
    hist = {}
    worst_flip = [0.0]
    seen = set()
    checked = 0
    for idx, ((ho, to), (hn, tn)) in enumerate(zip(old, new)):
        if (ho, hn) in seen:
            continue
        seen.add((ho, hn))
        if to == tn:
            same_text += 1
            continue
        # the wide-register form of a key: the generator takes it from the key's bit, which the listtext keys do not carry, so also run the texts with registers outside 0..1 as an extra
        # (the texts of a wide key come out of the same code with a wider range: not part of this list's keys)
        inp = Inputs(to, n, seed + idx, False)
        co, do = run(to, inp)
        inp2 = Inputs(to, n, seed + idx, False)
        cn, dn = run(tn, inp2)
        ok = ~do & ~dn
        lo = np.floor(np.clip(co, 0, 1) * 255 + 0.5)
        # RAW=1 (the FINCLAMP runs): the new text is compared as it leaves the shader, without the output conversion's saturation that would hide a clamp dropped wrongly
        ln = np.floor((cn if os.environ.get('RAW') else np.clip(cn, 0, 1)) * 255 + 0.5)
        diff = np.abs(lo - ln)[ok].max() if ok.any() else 0
        dd = int((do != dn).sum())
        lvl = np.abs(lo - ln).max(axis=1)   # (per fragment: the worst of its four channels)
        pix = (lvl > 0) & ok
        tot_frag += n; tot_diff += int(pix.sum()); tot_disc += dd
        for lv, cnt in zip(*np.unique(lvl[ok], return_counts=True)): hist[int(lv)] = hist.get(int(lv), 0) + int(cnt)
        worst_all = max(worst_all, diff)
        checked += 1
        flipDist = 0.0
        if dd:   # a flipped discard must be a fragment whose alpha sits on a rounding edge (mode 2 only): its distance to the nearest half step, in levels
            fl = (do != dn)
            av = np.clip(co[..., 3], 0, 1).astype(np.float64) * 255.0
            flipDist = float(np.abs(av[fl] - np.floor(av[fl]) - 0.5).max())
            worst_flip[0] = max(worst_flip[0], flipDist)
        if diff > tol or (dd and (tol == 0 or flipDist > 2e-3)):
            bad += 1
            print('DIFF key %s -> %s: max level difference %d, discard differs %d of %d' % (ho, hn, diff, dd, n))
    print('fragments %d: colour/alpha output differs by a level in %d (%.4f %%), discard decision differs in %d (%.4f %%); level histogram %s; worst distance of a flipped discard from a rounding edge %.2e levels' % (tot_frag, tot_diff, 100.0 * tot_diff / max(tot_frag, 1), tot_disc, 100.0 * tot_disc / max(tot_frag, 1), sorted(hist.items()), worst_flip[0]))
    print('keys: %d texts checked (changed), %d unchanged, %d failed; worst level difference %d; %d fragments per key' % (checked, same_text, bad, worst_all, n))
    sys.exit(1 if bad else 0)


main()
