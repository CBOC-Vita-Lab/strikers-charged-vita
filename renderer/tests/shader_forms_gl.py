#!/usr/bin/env python3
"""MSC_VGX_FS_HALF, MSC_VGX_TEX2D: the texts of `shader_forms dump` on two real front ends.
   shader_forms_gl.py DIR [TRANSLATOR]
 * Mesa (GLES2, surfaceless llvmpipe): every text of every form compiles and links; the half text also with VITAGL defined (half types spelled as vec types); each form draws the same
   pixels as the off text (Mesa has no half, so this proves the text's logic, not half's rounding: shader_forms.cpp emulates that).
 * vitaGL's real translator (built on the host from the tree, TRANSLATOR): the half names reach the compiler as half types, no half operand reaches vglMul, and half4 varyings get their semantics.
Exit status 0: passed; 77: no GL on this machine for the Mesa part.
"""
import ctypes as C, os, random, re, struct, subprocess, sys
from pathlib import Path

GL_VS, GL_FS = 0x8B31, 0x8B30
def egl():
    os.environ.setdefault("EGL_PLATFORM", "surfaceless")
    os.environ.setdefault("LIBGL_ALWAYS_SOFTWARE", "1")
    try:
        E = C.CDLL("libEGL.so.1"); G = C.CDLL("libGLESv2.so.2")
    except OSError:
        return None
    E.eglGetDisplay.restype = C.c_void_p; E.eglCreateContext.restype = C.c_void_p; E.eglCreatePbufferSurface.restype = C.c_void_p
    d = E.eglGetDisplay(None)
    if not d or not E.eglInitialize(C.c_void_p(d), None, None): return None
    E.eglBindAPI(0x30A0)
    attrs = (C.c_int * 7)(0x3033, 1, 0x3040, 4, 0x3024, 8, 0x3038)  # pbuffer, ES2, red 8
    cfg = C.c_void_p(); n = C.c_int()
    if not E.eglChooseConfig(C.c_void_p(d), attrs, C.byref(cfg), 1, C.byref(n)) or n.value == 0: return None
    surf = E.eglCreatePbufferSurface(C.c_void_p(d), cfg, (C.c_int * 5)(0x3057, 16, 0x3056, 16, 0x3038))
    ctx = E.eglCreateContext(C.c_void_p(d), cfg, None, (C.c_int * 3)(0x3098, 2, 0x3038))
    if not ctx or not E.eglMakeCurrent(C.c_void_p(d), C.c_void_p(surf), C.c_void_p(surf), C.c_void_p(ctx)): return None
    return G

def compile_(G, kind, text):
    s = G.glCreateShader(kind)
    b = C.c_char_p(("#version 100\n" + text).encode())
    G.glShaderSource(s, 1, C.byref(b), None)
    G.glCompileShader(s)
    ok = C.c_int(); G.glGetShaderiv(s, 0x8B81, C.byref(ok))
    if ok.value: return s, ""
    log = C.create_string_buffer(2000); G.glGetShaderInfoLog(s, 2000, None, log)
    return 0, log.value.decode()

ATTRS = ["a_pos", "a_nrm", "a_col0", "a_col1"] + ["a_tex%d" % i for i in range(8)] + ["a_tmi0", "a_tmi1", "a_bix", "a_bw"]
def link(G, vs, fs):
    p = G.glCreateProgram()
    G.glAttachShader(p, vs); G.glAttachShader(p, fs)
    for i, a in enumerate(ATTRS): G.glBindAttribLocation(p, i, a.encode())
    G.glLinkProgram(p)
    ok = C.c_int(); G.glGetProgramiv(p, 0x8B82, C.byref(ok))
    if ok.value: return p, ""
    log = C.create_string_buffer(2000); G.glGetProgramInfoLog(p, 2000, None, log)
    return 0, log.value.decode()

def set_uniforms(G, p, facts=0):
    n = C.c_int(); G.glGetProgramiv(p, 0x8B86, C.byref(n))
    for i in range(n.value):
        name = C.create_string_buffer(64); size = C.c_int(); ty = C.c_uint()
        G.glGetActiveUniform(p, i, 64, None, C.byref(size), C.byref(ty), name)
        base = name.value.decode().split("[")[0]
        loc = G.glGetUniformLocation(p, base.encode())
        rnd = random.Random(base)
        if base == "u_proj":
            G.glUniformMatrix4fv(loc, 1, 0, (C.c_float * 16)(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1))
        elif ty.value == 0x8B5E:  # sampler
            G.glUniform1i(loc, int(base[5:]))
        elif base in ("u_mv", "u_pal", "u_bone"):
            rows = [(1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0)]
            G.glUniform4fv(loc, size.value, (C.c_float * (size.value * 4))(*[v for j in range(size.value) for v in rows[j % 3]]))
        else:
            v = [rnd.random() for _ in range(size.value * 4)]
            if base == "u_lt":   # (dim colours: lights that saturate at 1 would hide a wrong term)
                for j in range(size.value):
                    if j < 2 or (j >= 4 and (j - 4) % 5 == 2):
                        for c in range(4): v[j * 4 + c] *= 0.1
            if base == "u_lt" and facts:   # MSC_VGX_VS_TRIM's facts: cos attenuation (1, 0, 0), and with 2 distance attenuation (1, 0, 0); light colours and ambient are 8-bit, 0..1 here
                for l in range((size.value - 4) // 5):
                    b = (4 + l * 5) * 4
                    v[b + 12:b + 15] = [1.0, 0.0, 0.0]
                    if facts == 2: v[b + 16:b + 19] = [1.0, 0.0, 0.0]
            if base == "u_tev" and size.value > 8: v[32] = float(rnd.randrange(256)); v[33] = float(rnd.randrange(256))   # (the alpha test's references are whole numbers, MSC_VGX_FS_TRIM relies on it)
            G.glUniform4fv(loc, size.value, (C.c_float * (size.value * 4))(*v))

def draw(G, p, facts=0):
    G.glUseProgram(p); set_uniforms(G, p, facts)
    G.glClear(0x4000)
    quad = (C.c_float * 8)(-1, -1, 1, -1, -1, 1, 1, 1)
    G.glBindBuffer(0x8892, 0)
    for i in range(16): G.glDisableVertexAttribArray(i); G.glVertexAttrib4f(i, C.c_float(0.25 + i / 32), C.c_float(0.5), C.c_float(0.75), C.c_float(0.0 if i == 0 else 1.0))
    G.glEnableVertexAttribArray(0)
    G.glVertexAttribPointer(0, 2, 0x1406, 0, 0, quad)
    G.glDrawArrays(5, 0, 4)
    px = (C.c_ubyte * (16 * 16 * 4))()
    G.glReadPixels(0, 0, 16, 16, 0x1908, 0x1401, px)
    return bytes(px)

def textures(G):
    rnd = random.Random(5)
    data = bytes(rnd.randrange(256) for _ in range(4 * 8 * 8))
    for u in range(8):
        t = C.c_uint(); G.glGenTextures(1, C.byref(t)); G.glActiveTexture(0x84C0 + u); G.glBindTexture(0x0DE1, t)
        G.glTexImage2D(0x0DE1, 0, 0x1908, 8, 8, 0, 0x1908, 0x1401, data)
        G.glTexParameteri(0x0DE1, 0x2801, 0x2600); G.glTexParameteri(0x0DE1, 0x2800, 0x2600)

def main():
    d = Path(sys.argv[1]); fail = 0
    names = sorted({p.name[:-3] for p in d.glob("*.fs")}, key=lambda n: (int(n.split(".")[0]), n.replace("voff", "aoff")))
    def bad(what):
        nonlocal fail
        fail += 1
        if fail < 12: print("FAIL:", what[:600])
    if len(sys.argv) > 2:
        tr = sys.argv[2]; n = 0
        for name in names:
            for ext, kind in ((".fs", "f"), (".vs", "v")):
                r = subprocess.run([tr, str(d / (name + ext)), kind], capture_output=True, text=True)
                if r.returncode: bad("translator failed on " + name + ext); continue
                n += 1
                body = r.stdout[r.stdout.index("varying out float4 gl_FragColor"):]
                body = body[body.index("#endif"):]
                if "HV3" in body or "HV4" in body or "HF" in body: bad(name + ": a half name was not expanded")
                for line in body.splitlines():
                    if "vglMul" in line and "half" in line: bad(name + ": half operand reaches vglMul: " + line)
                if ext == ".fs" and ".half" in name:
                    if "half4 prev=half4(" not in body and "half4 prev = half4(" not in body: bad(name + ": no half TEV registers")
                    want = "VIN(half4 v_col" if "halfvar" in name else "VIN(vec4 v_col"
                    if "v_col" in (d / (name + ext)).read_text() and want not in body: bad(name + ": colour varying form")
                if ".tex2" in name or "_tex2" in name:
                    if re.search(r"VIN\(vec3 v_tex", body) and ext == ".fs" and not re.search(r"v_tex\d+\.z", body): bad(name + ": a vec3 varying without a z read")
        print("vitaGL translator: %d texts" % n)
    G = egl()
    if not G:
        print("no EGL/GLES2 here: the Mesa part did not run"); return 77 if not fail else 1
    textures(G)
    linked = drawn = 0
    off = {}
    vfacts = {}; vchecked = [0, 0]; vstat = [0, 0, 0]
    half_defs = "#define VITAGL\n#define half float\n#define half3 vec3\n#define half4 vec4\n"
    for name in names:
        vt, ft = (d / (name + ".vs")).read_text(), (d / (name + ".fs")).read_text()
        v, e = compile_(G, GL_VS, vt)
        if not v: bad(name + ".vs: " + e + vt); continue
        f, e = compile_(G, GL_FS, ft)
        if not f: bad(name + ".fs: " + e + ft); continue
        if ".half" in name or "half" in name.split(".")[1]:
            f2, e = compile_(G, GL_FS, half_defs + ft)
            if not f2: bad(name + ".fs (VITAGL branch): " + e)
        if ".nolink" in name: continue
        p, e = link(G, v, f)
        if not p: bad(name + " link: " + e); continue
        linked += 1
        idx = name.split(".")[0]
        form = name.split(".")[1]
        if form == "vunread":   # (no colour 1 in the vertex shader: it links with a fragment shader that does not read it)
            G.glDeleteProgram(p)
            continue
        if form in ("voff", "vtrim", "vcos"):
            if form == "voff":
                vfacts[idx] = (draw(G, p, 1), draw(G, p, 2), draw(G, p, 0))
                vstat[0] += vfacts[idx][1] != vfacts[idx][2]; vstat[1] += len(set(vfacts[idx][2][i:i+4] for i in range(0, 1024, 4))) > 1; vstat[2] += any(vfacts[idx][2])
            elif idx in vfacts:
                want = vfacts[idx][1 if form == "vtrim" else 0]
                got = draw(G, p, 2 if form == "vtrim" else 1)
                vchecked[0] += 1
                if got != want: bad(name + ": other pixels than the plain vertex text with the same light values")
                if draw(G, p, 0) != vfacts[idx][2]: vchecked[1] += 1
            G.glDeleteProgram(p)
            continue
        px = draw(G, p)
        if name.endswith(".off"): off[idx] = px
        elif idx in off:
            drawn += 1
            if px != off[idx]: bad(name + ": other pixels than the off text")
        G.glDeleteProgram(p)
    print("Mesa: %d texts compiled, %d pairs linked, %d forms drew the off pixels" % (len(names) * 2, linked, drawn))
    print("MSC_VGX_VS_TRIM: %d texts drew the plain text's pixels; %d of them differ when the facts do not hold (the deliberate break)" % tuple(vchecked))
    print("plain text: changes with the facts %d, varied picture %d, not black %d" % tuple(vstat))
    if vchecked[0] and not vchecked[1]: bad("the vertex trim's break was not seen")
    print("shader forms (GL): " + ("passed" if not fail else "%d FAILED" % fail))
    return 1 if fail else 0

sys.exit(main())
