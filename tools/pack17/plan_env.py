#!/usr/bin/env python3
# Prints the Vita plan's kSpeed + kQuality defaults (vita_plan.cpp) as KEY=VAL words, for the host harness (which does not run the plan).
import re,sys
src=open(sys.argv[1]+"/native/platform/vita/src/vita_plan.cpp").read()
out=[]
for name in ("kSpeed","kQuality"):
    m=re.search(r"%s\[\]\[2\] = \{(.*?)\};"%name,src,re.S)
    body=re.sub(r"//[^\n]*","",m.group(1))
    for k,v in re.findall(r'\{"(MSC_[A-Z0-9_]+)",\s*"([^"]*)"\}',body): out.append("%s=%s"%(k,v))
print(" ".join(out))
