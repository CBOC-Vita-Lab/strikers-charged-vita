#!/usr/bin/env python3
"""VRS (wave 4): per-segment means of the renderer's per-frame counts and the render thread's profile from one Vita3K / console run.

  python3 -I platform/vita/tools/vrs_census.py <run dir>      (a job_v3k.sh result: log.txt, glstats.csv, vgx_frames.csv; a null-backend run
                                                              has no glstats.csv: pass its err.log as log.txt)

Inputs (env-np-vrs-match.txt): MSC_VGX_GLSTATS (GL call census, one row per executed frame), MSC_VGX_FRAMESTATS (front end: draws, vertices,
triangles, texture uploads), MSC_VGX_PROF=2 ("[vgx] prof" lines every 100 executed frames) and the "[nav]" lines of the game log, which
give the segment boundaries by frame number. Prints a table; no file is written.
"""
import csv, os, re, sys

def read_csv(path):
    if not os.path.exists(path):
        return []
    with open(path, newline='') as f:
        r = csv.DictReader(f)
        rows = []
        for row in r:
            try:
                rows.append({k: (int(v) if re.fullmatch(r'-?\d+', v or '') else v) for k, v in row.items() if k})
            except Exception:
                pass
        return rows

def segments(log):
    """[(first frame, name)] from the [nav] lines of the game log."""
    marks = [(0, 'boot')]
    for line in open(log, errors='replace'):
        m = re.match(r'\[nav\] f=(\d+) (.*)', line)
        if not m:
            continue
        f, t = int(m.group(1)), m.group(2)
        if 'movieplayer' in t and 'scenes' in t:
            marks.append((f, 'movie'))
        elif 'sms2_start' in t:
            marks.append((f, 'title'))
        elif re.search(r'scenes: .*main_menu|scenes: .*choose_|scenes: .*options|scenes: .*domination', t) and 'loadingscreen' not in t:
            if marks[-1][1] != 'menus':
                marks.append((f, 'menus'))
        elif 'loadingscreen' in t:
            marks.append((f, 'loading'))
        elif 'match state 1 -> 5' in t:
            marks.append((f, 'open play'))
        elif 'match state 5 -> 3' in t:
            marks.append((f, 'full time'))
    return marks

def seg_of(marks, f):
    name = marks[0][1]
    for mf, n in marks:
        if f >= mf:
            name = n
    return name

def main():
    d = sys.argv[1]
    log = os.path.join(d, 'log.txt')
    gl = read_csv(os.path.join(d, 'glstats.csv'))
    fs = {r['frame']: r for r in read_csv(os.path.join(d, 'vgx_frames.csv'))}
    marks = segments(log) if os.path.exists(log) else [(0, 'all')]
    print('segments:', ', '.join('%s@%d' % (n, f) for f, n in marks))
    cols = ['glCalls', 'scenes', 'efbScenes', 'fboBinds', 'clears', 'copyQuads', 'draws', 'drawIndices', 'programUses', 'uniformCalls', 'uniformBytes',
            'ringBytes', 'streamUploads', 'streamBytes', 'texUploads', 'texUploadBytes', 'texBinds', 'vboBinds', 'attrPointers', 'scissorUpdates',
            'viewportCalls', 'stateCalls', 'fragSwitches']
    if not gl:   # a null-backend run (host32 / armhf) has only the front-end csv: its frames stand in, the GL columns stay empty
        gl = [{'frame': f} for f in sorted(fs)]
    acc = {}
    for r in gl:
        f = r['frame']
        s = seg_of(marks, f)
        a = acc.setdefault(s, {'n': 0})
        a['n'] += 1
        for c in cols:
            a[c] = a.get(c, 0) + r.get(c, 0)
        x = fs.get(f)
        if x:
            a['verts'] = a.get('verts', 0) + x['verts']
            a['tris'] = a.get('tris', 0) + x['tris']
            a['fe_texUploads'] = a.get('fe_texUploads', 0) + x['texUploads']
            a['fe_texUploadBytes'] = a.get('fe_texUploadBytes', 0) + x['texUploadBytes']
    order = [n for _, n in marks]
    seen = []
    for n in order:
        if n in acc and n not in seen:
            seen.append(n)
    allcols = cols + ['verts', 'tris', 'fe_texUploads', 'fe_texUploadBytes']
    print('%-18s' % 'per frame (mean)' + ''.join('%12s' % s for s in seen))
    print('%-18s' % 'frames' + ''.join('%12d' % acc[s]['n'] for s in seen))
    for c in allcols:
        print('%-18s' % c + ''.join('%12.1f' % (acc[s].get(c, 0) / acc[s]['n']) for s in seen))
    # the render thread's own profile lines (every 100 executed frames), labelled by the segment of the window's first frame
    print()
    print('render-thread profile lines (ms per frame over 100 executed frames):')
    k = 0
    for line in (open(log, errors='replace') if os.path.exists(log) else []):
        if line.startswith('[vgx] prof'):
            first = gl[k * 100]['frame'] if k * 100 < len(gl) else -1
            print('  window %2d (frames from %5d, %s): %s' % (k, first, seg_of(marks, first) if first >= 0 else '?', line.strip()[:400]))
            k += 1

main()
