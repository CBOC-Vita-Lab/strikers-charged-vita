#!/usr/bin/env python3
"""Compile every Vita unit syntax-only with clang -Wformat so printf-family argument bugs show (the game units are
normally built with -w): %llu / %lld with a 32-bit argument, a missing argument, one conversion too many.
Only functions the compiler knows as printf-like (libc, or declared with __attribute__((format))) are checked;
tools/check_printf_args.py covers the project's other log wrappers by argument count.

Usage:
  1. dry run of the console build to get its compile lines (nothing is built):
       cd <worktree> && make -n -B -k -C native TARGET=vita WITH_SDK=1 WITH_GX=1 BUILD_ROOT=/tmp/x GX_VGX=<worktree>/renderer \
         VDEPS=<deps prefix> KUBRIDGE=/deps/sources/bythos14_kubridge NP_MBEDTLS=/tls/mbedtls/vita LTO=thin NP_PLAYER=1 NP_LOGGING=1 \
         NP_TITLE_ID=CDXMP0001 NP_APP_VER=01.30 OPT=-O2 DEBUG=-g0 vpk > dry.log
  2. python3 tools/check_printf_clang.py dry.log <worktree>/native [--only substring]   (one unit at a time, nice it yourself)
Exit 1 when a format warning is found. Output: one line per distinct warning (file:line: text).
"""
import re, shlex, subprocess, sys, os

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    only = None
    if "--only" in sys.argv:
        only = sys.argv[sys.argv.index("--only") + 1]
        args = [a for a in args if a != only]
    dry, cwd = args[0], args[1]
    seen, units = set(), []
    for ln in open(dry, errors="replace"):
        if not re.match(r"(clang\+\+|clang|/opt/vitasdk/bin/arm-vita-eabi-(g\+\+|gcc)) ", ln) or " -c " not in ln:
            continue
        ln = re.sub(r"\s2>\s*\S+.*$", "", ln.rstrip())
        t = shlex.split(ln)
        if "-c" not in t:
            continue
        src = t[t.index("-c") + 1]
        if src in seen or (only and only not in src):
            continue
        seen.add(src)
        units.append((src, t))
    warn, bad = {}, []
    for src, t in units:
        p = src if os.path.isabs(src) else os.path.join(cwd, src)
        try:
            text = open(p, errors="replace").read()
        except OSError:
            continue
        if not re.search(r'"[^"\n]*%[-+ #0-9.*]*(?:l|ll|h|hh|z|j|t)?[diouxXcsfeEgGp]', text):
            continue
        out, skip = [], 0
        for i, a in enumerate(t):
            if skip: skip -= 1; continue
            if a in ("-o", "-MF"): skip = 1; continue
            if a in ("-MMD", "-MP", "-w", "-fno-aggressive-loop-optimizations", "-fno-lifetime-dse") or a.startswith("-flto") or a.startswith("-g"): continue
            out.append(a)
        cc = out[0]
        if "arm-vita-eabi" in cc:
            cc = "clang++" if cc.endswith("g++") else "clang"
            out = [cc, "--target=armv7a-none-eabihf", "-nostdlibinc", "-D__vita__", "-D__USES_INITFINI__", "-femulated-tls", "-fno-pic",
                   "-isystem", "/opt/vitasdk/arm-vita-eabi/include/c++/15.2.0", "-isystem", "/opt/vitasdk/arm-vita-eabi/include/c++/15.2.0/arm-vita-eabi",
                   "-isystem", "/opt/vitasdk/arm-vita-eabi/include/c++/15.2.0/backward", "-isystem", "/opt/vitasdk/arm-vita-eabi/include",
                   "-Wno-psabi", "-Wno-unknown-warning-option"] + out[1:]
        out += ["-fsyntax-only", "-Wformat", "-Wformat-extra-args", "-Wformat-insufficient-args", "-Wno-format-nonliteral", "-ferror-limit=0"]
        r = subprocess.run(out, cwd=cwd, capture_output=True, text=True)
        if r.returncode != 0 and "error:" in r.stderr:
            bad.append((src, next(l for l in r.stderr.splitlines() if "error:" in l)))
        for l in r.stderr.splitlines():
            m = re.match(r"(.+?):(\d+):\d+: warning: (.*\[-Wformat[^\]]*\])", l)
            if m and not m.group(1).startswith("/opt/vitasdk") and "/deps/" not in m.group(1):
                warn.setdefault((m.group(1), m.group(2), m.group(3)), src)
    for (f, ln, msg), src in sorted(warn.items()):
        print(f"{f}:{ln}: {msg}")
    for src, l in bad:
        print(f"NOT CHECKED (compile error, fix the flags): {src}: {l[:160]}")
    print(f"checked {len(units)} unit lines; {len(warn)} distinct format warnings")
    sys.exit(1 if warn or bad else 0)

main()
