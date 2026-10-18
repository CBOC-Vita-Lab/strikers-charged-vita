#!/usr/bin/env python3
"""gen_profiles.py: PSVshellPlus per-title profile files for the native port (PIPE, wave 5). NOTHING here installs anything.

A PSVshellPlus profile is one 28-byte file named after the title id, in ur0:data/PSVshell/profiles/ (PSVshellPlus_Shell/profile.h, profile.cpp):
    char     ver[8]      "PSVS0300"
    uint32   lock        bit 0 CPU, 1 GPU, 2 bus, 3 GPU xbar: the clocks the application may NOT change (its own scePower*Set*ClockFrequency is ignored)
    int32    clock[4]    CPU, GPU, bus, GPU xbar in MHz  (the shell applies CPU, GPU and xbar when the application comes to the front; the bus value is read but not applied)
all little endian. The shell loads <title id> when that application becomes the active one, calls psvsSetArmClockFrequency(clock[0]); 500 is
the special case that the kernel part turns into scePervasiveArmClockSelect(15, 16); then it locks what `lock` says, so that the game's own
scePowerSetArmClockFrequency(444) (np_vita_main.cpp SetUpCpu) does not bring the clock back.

  gen_profiles.py [--out DIR] [--cpu 500] [--gpu 222] [--bus 222] [--xbar 166] [--lock vitadhoc|cpu|all|none] [--check FILE]
writes DIR/MSCN00001 (lab build), MSCN00002 (smoke application), MSCN00003 (player build), MSCH00002 (release build: the recompiled app's id).
Default lock is `vitadhoc` (0x0b: CPU, GPU and xbar locked), byte for byte the file the VitAdhoc session measured on the owner's console (500/222/222/166, 496 MHz
measured, about 10 % less CPU time per frame than at 444); `cpu` (0x01) locks only the CPU clock, whose own request the game makes (444).
--check FILE prints a profile file's fields (to verify what is on the console after a download)."""
import argparse, os, struct, sys

TITLES = ['MSCN00001', 'MSCN00002', 'MSCN00003', 'MSCH00002']
LOCK = {'none': 0, 'cpu': 1, 'all': 15, 'vitadhoc': 0x0b}   # vitadhoc = CPU + GPU + xbar: the flags of the profile measured on the owner's Vita (VitAdhoc, 496 MHz)

def pack(cpu, gpu, bus, xbar, lock):
    return struct.pack('<8sI4i', b'PSVS0300', lock, cpu, gpu, bus, xbar)

def show(path):
    data = open(path, 'rb').read()
    if len(data) != 28:
        print('%s: %d bytes (a profile is 28)' % (path, len(data))); return 1
    ver, lock, cpu, gpu, bus, xbar = struct.unpack('<8sI4i', data)
    print('%s: ver %r lock 0x%X (cpu %d gpu %d bus %d xbar %d locked) clocks cpu %d gpu %d bus %d xbar %d MHz' % (
        path, ver, lock, lock & 1, (lock >> 1) & 1, (lock >> 2) & 1, (lock >> 3) & 1, cpu, gpu, bus, xbar))
    return 0 if ver == b'PSVS0300' else 1

ap = argparse.ArgumentParser()
ap.add_argument('--out', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'profiles'))
ap.add_argument('--cpu', type=int, default=500); ap.add_argument('--gpu', type=int, default=222)
ap.add_argument('--bus', type=int, default=222); ap.add_argument('--xbar', type=int, default=166)
ap.add_argument('--lock', choices=sorted(LOCK), default='vitadhoc')
ap.add_argument('--check')
a = ap.parse_args()
if a.check:
    sys.exit(show(a.check))
os.makedirs(a.out, exist_ok=True)
blob = pack(a.cpu, a.gpu, a.bus, a.xbar, LOCK[a.lock])
assert len(blob) == 28
for t in TITLES:
    with open(os.path.join(a.out, t), 'wb') as f:
        f.write(blob)
    show(os.path.join(a.out, t))
