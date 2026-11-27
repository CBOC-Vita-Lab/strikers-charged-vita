"""pl.py: picture-look helpers (python3 -I; numpy, PIL, skimage). Host emulation of the present shaders of renderer/src/vgx_backend_gl.cpp
(PictureFragmentSource / DrawPicture) so that sharpening strength etc. can be varied without a new game run; validated against the GL output (validate.py)."""
import numpy as np
from PIL import Image
from math import sqrt
DW, DH = 960, 544

def load(p):
    return np.asarray(Image.open(p).convert('RGB'), dtype=np.float32) / 255.0

def save(a, p):
    Image.fromarray(np.clip(np.rint(a * 255.0), 0, 255).astype(np.uint8)).save(p)

def _bil(img, px, py):
    """bilinear fetch at texel-space coordinates (px, py) (texel k covers [k, k+1)), clamp to edge. px: (W,), py: (H,) -> (H,W,3)"""
    h, w = img.shape[:2]
    fx = px - 0.5; fy = py - 0.5
    x0 = np.floor(fx); y0 = np.floor(fy)
    ax = (fx - x0).astype(np.float32); ay = (fy - y0).astype(np.float32)
    x0i = np.clip(x0.astype(int), 0, w - 1); x1i = np.clip(x0.astype(int) + 1, 0, w - 1)
    y0i = np.clip(y0.astype(int), 0, h - 1); y1i = np.clip(y0.astype(int) + 1, 0, h - 1)
    r0 = img[y0i][:, x0i] * (1 - ax)[None, :, None] + img[y0i][:, x1i] * ax[None, :, None]
    r1 = img[y1i][:, x0i] * (1 - ax)[None, :, None] + img[y1i][:, x1i] * ax[None, :, None]
    return r0 * (1 - ay)[:, None, None] + r1 * ay[:, None, None]

def coords(img, inset, dw=DW, dh=DH):
    h, w = img.shape[:2]
    a = (np.arange(dw) + 0.5) / dw; b = (np.arange(dh) + 0.5) / dh
    return inset + a * (w - 2 * inset), inset + b * (h - 2 * inset)

def linear(img, dw=DW, dh=DH):
    px, py = coords(img, 0, dw, dh)
    return np.clip(_bil(img, px, py), 0, 1)

def combined(img, k_sharpen, dw=DW, dh=DH, edge=True, gain=1.4, inset=0, E=8.0, O=0.3):
    """PIC_COMBINED; edge=False: the same without the edge term (t = 0, i.e. an unsharp mask everywhere = FAST with gain 1.4)"""
    px, py = coords(img, inset, dw, dh)
    c = _bil(img, px, py); a = _bil(img, px + 0.5, py + 0.5); b = _bil(img, px - 0.5, py - 0.5)
    d = c - (a + b) * 0.5
    if edge:
        t = np.clip((np.abs(a[..., 1] - b[..., 1]) + np.abs(d[..., 1]) * 2.0) * E - O, 0, 1)[..., None]
    else:
        t = 0.0
    return np.clip(c + d * ((1 - t) * k_sharpen * gain), 0, 1)

def fast(img, k_sharpen, dw=DW, dh=DH, inset=0):
    px, py = coords(img, inset, dw, dh)
    c = _bil(img, px, py); a = _bil(img, px + 0.5, py + 0.5); b = _bil(img, px - 0.5, py - 0.5)
    return np.clip(c + (c - (a + b) * 0.5) * k_sharpen, 0, 1)

def stretch(w, h):
    return sqrt((DW / w) * (DH / h))

def sharpen_for(w, h, k=1.0, cap=0.8):
    s = stretch(w, h)
    return min(cap, k * sqrt(max(s - 1.0, 0.0)))

# ---- metrics against the native picture
def luma(a):
    return a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114

def psnr(a, b):
    m = float(((a - b) ** 2).mean())
    return 99.0 if m <= 0 else -10 * np.log10(m)

def ssim(a, b):
    from skimage.metrics import structural_similarity as S
    return float(S(luma(a), luma(b), data_range=1.0))

def gradE(a):
    l = luma(a)
    gx = np.diff(l, axis=1)[:-1, :]; gy = np.diff(l, axis=0)[:, :-1]
    return float(np.sqrt(gx * gx + gy * gy).mean())

def shimmer(v0, v1, n0, n1, thr=2 / 255.0):
    """mean |temporal change| of the variant where the native picture is static (|dN| <= thr), over that of the native picture there (floor)"""
    dn = np.abs(luma(n1) - luma(n0)); dv = np.abs(luma(v1) - luma(v0))
    m = dn <= thr
    return float(dv[m].mean()), float(dn[m].mean()), float(m.mean())

def hf_ratio(a, n):
    """energy of the high band (picture minus its 3x3 box blur), against native: <1 soft, >1 over-sharp"""
    from scipy.ndimage import uniform_filter
    def hf(x):
        l = luma(x); return float(np.sqrt(((l - uniform_filter(l, 3)) ** 2).mean()))
    return hf(a) / hf(n)

def overshoot(a, n, win=5):
    """ringing / halo: mean (in 8-bit levels) by which the picture leaves the range of the native picture's own win x win neighbourhood (luma)"""
    from scipy.ndimage import maximum_filter, minimum_filter
    la, ln = luma(a), luma(n)
    hi = maximum_filter(ln, win); lo = minimum_filter(ln, win)
    return float((np.maximum(la - hi, 0) + np.maximum(lo - la, 0)).mean() * 255.0)

# ---- quantisation: plain rounding (what an 8-bit target does) and an ordered dither added before it
_B4 = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], dtype=np.float32)
def quant(a, dither=None):
    """a: float picture 0..1. dither None: round. 'bayer': 4x4 Bayer threshold (+-0.5 LSB). 'ign': interleaved gradient noise (Jimenez), one mad + fract + fract"""
    h, w = a.shape[:2]
    v = a * 255.0
    if dither == 'bayer':
        t = (np.tile(_B4, (h // 4 + 1, w // 4 + 1))[:h, :w] + 0.5) / 16.0 - 0.5
        v = v + t[..., None]
    elif dither == 'ign':
        y, x = np.mgrid[0:h, 0:w].astype(np.float32)
        f = (52.9829189 * ((0.06711056 * x + 0.00583715 * y) % 1.0)) % 1.0
        v = v + (f - 0.5)[..., None]
    return np.clip(np.rint(v), 0, 255).astype(np.float32) / 255.0

def band_err(q, f, sigma=2.5):
    """low-frequency part of the quantisation error (what banding is): rms (levels) of gaussian_blur(q - f)"""
    from scipy.ndimage import gaussian_filter
    e = (q - f) * 255.0
    return float(np.sqrt(np.mean([gaussian_filter(e[..., c], sigma) ** 2 for c in range(3)])))

def smooth(img, k_sharpen, dw=DW, dh=DH, edge=1.0):
    """PIC_SMOOTH (6 reads; the 1.1 default): unsharp mask from the 4 neighbours + FXAA-like sub-pixel step across contrast edges. No inset, clamp to texel centres."""
    h, w = img.shape[:2]
    px, py = coords(img, 0, dw, dh)
    lo_x, hi_x, lo_y, hi_y = 0.5, w - 0.5, 0.5, h - 0.5
    cl = lambda p, lo, hi: np.clip(p, lo, hi)
    PX, PY = cl(px, lo_x, hi_x), cl(py, lo_y, hi_y)
    c = _bil(img, PX, PY)
    s1 = _bil(img, cl(PX + 1, lo_x, hi_x), PY); s2 = _bil(img, cl(PX - 1, lo_x, hi_x), PY)
    s3 = _bil(img, PX, cl(PY + 1, lo_y, hi_y)); s4 = _bil(img, PX, cl(PY - 1, lo_y, hi_y))
    b = (s1 + s2 + s3 + s4) * 0.25
    lw = np.array([0.299, 0.587, 0.114], dtype=np.float32)
    lC, lE, lW, lS, lN = [(x * lw).sum(-1) for x in (c, s1, s2, s3, s4)]
    lMax = np.maximum(np.maximum(np.maximum(lE, lW), np.maximum(lS, lN)), lC); lMin = np.minimum(np.minimum(np.minimum(lE, lW), np.minimum(lS, lN)), lC)
    rng = lMax - lMin
    on = (rng >= np.maximum(0.06, 0.15 * lMax)).astype(np.float32) * edge
    sub = np.clip(np.abs((lE + lW + lS + lN) * 0.25 - lC) / np.maximum(rng, 0.001), 0, 1)
    sm = sub * sub * (3 - 2 * sub); blend = sm * sm * 0.75 * on
    horz = (np.abs(lE + lW - 2 * lC) <= np.abs(lN + lS - 2 * lC)).astype(np.float32)   # step(a, b) = b >= a
    sy = (np.abs(lN - lC) <= np.abs(lS - lC)).astype(np.float32) * 2 - 1
    sx = (np.abs(lW - lC) <= np.abs(lE - lC)).astype(np.float32) * 2 - 1
    nx = (1 - horz) * sx; ny = horz * sy
    # a = texture(uv + nrm*blend) : per-pixel sample position
    qx = cl(PX[None, :] + nx * blend, lo_x, hi_x); qy = cl(PY[:, None] + ny * blend, lo_y, hi_y)
    a = _bil_pp(img, qx, qy)
    keep = 1 - 0.7 * on
    return np.clip(a + (c - b) * (k_sharpen * keep)[..., None], 0, 1)

def _bil_pp(img, px, py):
    """bilinear at per-pixel texel coordinates px, py (H,W)"""
    h, w = img.shape[:2]
    fx = px - 0.5; fy = py - 0.5
    x0 = np.floor(fx); y0 = np.floor(fy); ax = (fx - x0)[..., None]; ay = (fy - y0)[..., None]
    x0i = np.clip(x0.astype(int), 0, w - 1); x1i = np.clip(x0.astype(int) + 1, 0, w - 1)
    y0i = np.clip(y0.astype(int), 0, h - 1); y1i = np.clip(y0.astype(int) + 1, 0, h - 1)
    return (img[y0i, x0i] * (1 - ax) + img[y0i, x1i] * ax) * (1 - ay) + (img[y1i, x0i] * (1 - ax) + img[y1i, x1i] * ax) * ay

def shimmer2(v0, v1, n0, n1, thr=2 / 255.0, gthr=0.08):
    """(flat, edge): mean |temporal change| (8-bit levels) of the variant where the native picture is static, split into flat pixels and edge pixels (native luma gradient > gthr)"""
    from scipy.ndimage import maximum_filter
    ln = luma(n0)
    gx = np.abs(np.diff(ln, axis=1, append=ln[:, -1:])); gy = np.abs(np.diff(ln, axis=0, append=ln[-1:, :]))
    g = maximum_filter(np.maximum(gx, gy), 3)
    static = np.abs(luma(n1) - luma(n0)) <= thr
    static = static & ~(maximum_filter((np.abs(luma(n1) - luma(n0)) > thr).astype(np.uint8), 5) > 0)  # also the neighbourhood is static
    dv = np.abs(luma(v1) - luma(v0)) * 255
    e = static & (g > gthr); f = static & (g <= gthr)
    return float(dv[f].mean()), float(dv[e].mean()), float(f.mean()), float(e.mean())

def tdiff(v0, v1, n0, n1):
    """temporal behaviour over a pair of consecutive frames: (energy of the variant's change / energy of the native change, mean |dV - dN| / mean |dN|). Both near 1 / small: the variant moves like the native picture; a larger first number is extra flicker / crawl"""
    dv = luma(v1) - luma(v0); dn = luma(n1) - luma(n0)
    return float(np.abs(dv).mean() / max(np.abs(dn).mean(), 1e-9)), float(np.abs(dv - dn).mean() / max(np.abs(dn).mean(), 1e-9))
