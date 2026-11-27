#!/usr/bin/env python3
"""price17_sym.py <exe> <shim.txt> [thread-tid]  : resolve shim.txt site lines (exe+0xOFF) to function names with nm; prints S lines with names."""
import sys,subprocess,bisect,re
exe,shim=sys.argv[1],sys.argv[2]
syms=[]
for l in subprocess.run(["nm","-n","-C","--defined-only",exe],capture_output=True,text=True).stdout.splitlines():
    p=l.split(" ",2)
    if len(p)==3 and p[1] in "tTwW": syms.append((int(p[0],16),p[2]))
addrs=[a for a,_ in syms]
def name(off):
    i=bisect.bisect_right(addrs,off-1)-1
    return (syms[i][1][:70] if i>=0 else "?")+"+%d"%(off-syms[i][0]) if i>=0 else "?"
def res(tok):
    m=re.match(r"(.*)\+0x([0-9a-f]+)$",tok)
    if not m: return tok
    return name(int(m.group(2),16)) if m.group(1).endswith("glnull") else m.group(1).split("/")[-1]
for l in open(shim):
    p=l.split()
    if p[0]=="S": print(p[1],p[2],p[3],p[4],res(p[5]),"<-",res(p[6]),"<-",res(p[7]) if len(p)>7 else "")
    else: print(l.rstrip()[:400])
