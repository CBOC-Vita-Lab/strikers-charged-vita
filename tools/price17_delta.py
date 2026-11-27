#!/usr/bin/env python3
"""price17_delta.py <small.sym> <big.sym> <frames> : per-thread counts per picture between two runs (price17_sym.py output); threads matched by creation order."""
import sys,collections
def load(p):
    th=[];sites=[]
    for l in open(p):
        w=l.split()
        if not w: continue
        if w[1].isdigit() and "=" in l and not l.startswith("S"): pass
        if l.startswith(tuple("0123456789")) and " getenv " not in l and "=" in l: pass
    return None
def parse(p):
    T=[];S=collections.defaultdict(dict);idx={}
    for l in open(p):
        if l.startswith("T "):
            w=l.split(); tid=w[1]; idx[tid]=len(T); d={}
            for t in w[3:]:
                if "=" in t:
                    k,v=t.rsplit("=",1); d[k]=int(v)
            T.append((tid,w[2],d))
        elif l[0].isdigit():
            w=l.split(" ",3); tid=w[0]
            rest=l.rstrip().split(" ")
            # tid name kind... : name has no spaces; kind may contain none; count is the first numeric after kind
            tid=rest[0]; kind=rest[2]; n=int(rest[3]); site=" ".join(rest[4:])
            S[idx.get(tid,-1)][(kind,site)]=n
    return T,S
a,b,fr=sys.argv[1],sys.argv[2],float(sys.argv[3])
Ta,Sa=parse(a);Tb,Sb=parse(b)
for i,(tid,nm,d) in enumerate(Tb):
    da=Ta[i][2] if i<len(Ta) else {}
    row={k:(v-da.get(k,0))/fr for k,v in d.items() if v-da.get(k,0)>0}
    print("THREAD",i,tid,"per picture:"," ".join("%s=%.2f"%(k,v) for k,v in sorted(row.items(),key=lambda x:-x[1])))
    sites=[]
    for key,n in Sb.get(i,{}).items():
        dn=n-Sa.get(i,{}).get(key,0)
        if dn>0: sites.append((dn/fr,key))
    for v,(k,s) in sorted(sites,reverse=True)[:int(sys.argv[4]) if len(sys.argv)>4 else 8]: print("   %.2f/pic %s %s"%(v,k,s[:150]))
