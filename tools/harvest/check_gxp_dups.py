#!/usr/bin/env python3
"""check_gxp_dups.py <cache dir> [<cache dir> ...] : the same gxp file name from different harvest routes must be byte-identical (tools15, Galaxy's seedwalk collect.py rule).

One shader text = one hash = one file name <key>/v3/<f|v>/XX/<HASH>.gxp; the compiler is deterministic, so two routes that compiled the same text made the same bytes. Anything
else means a nondeterministic compile or a shader-text switch that differed between the routes: assemble_seed.sh / make_shader_pack.py would silently keep the first, so the
harvest stops instead. Every argument is one route's profile cache (the folder with <key>/v3/...); a missing folder is skipped.
Exit 0: no conflict (prints how many names were seen in more than one cache); exit 1: conflict lines "CONFLICT <name>: <cache a> != <cache b>"; exit 2: usage.
"""
import hashlib, os, sys


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def scan(roots):
    seen, shared, conflicts = {}, set(), []
    for root in roots:
        if not os.path.isdir(root):
            continue
        for dp, _dn, fn in os.walk(root):
            for n in fn:
                if not n.endswith(".gxp"):
                    continue
                p = os.path.join(dp, n)
                rel = os.path.relpath(p, root)
                d = digest(p)
                if rel in seen:
                    shared.add(rel)
                    if seen[rel][0] != d:
                        conflicts.append((rel, seen[rel][1], root))
                else:
                    seen[rel] = (d, root)
    return len(seen), len(shared), conflicts


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 2
    total, shared, conflicts = scan(argv)
    for rel, a, b in conflicts:
        print("CONFLICT %s: %s != %s" % (rel, a, b))
    print("gxp check: %d names, %d seen in more than one cache, %d conflicts" % (total, shared, len(conflicts)))
    return 1 if conflicts else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
