#!/usr/bin/env python3
"""task_budget.py: instruction counts per nlTask and per presented frame from a qemu run.

Input: the log of a run of the armhf build under qemu-arm with tools/qemu-plugin/libframecount.so
(`-plugin ...libframecount.so -d plugin`), MSC_QEMU_MARK=1 (one mark per presented frame) and
MSC_TASK_MARKS=1 (NpTaskBegin/NpTaskEnd marks). Lines used:
    [insn] mark frame=N caller=C t0=... t1=... t2=...   cumulative instructions per guest thread
    [taskmarks] I=name                                  task index -> name (printed once)
    [ft] <frame> <sim steps so far> <retraces> <match state>   (MSC_FRAME_TRACE=1, optional)
Task marks are N >= 0x20000000: index (N - 0x20000000) >> 1, odd = end.

Output: per task, over the window [--from, --to] of presented frames: calls, and the mean / median /
p95 / max of the game thread's (t0) instructions per frame in M, plus the other threads' totals;
then per-frame totals (frame mark to frame mark) and, with [ft] lines, split by simulation steps
in the frame. Numbers are instruction counts of the guest under qemu, not time.

  task_budget.py run.log [--from 600] [--to 1200] [--thread 0]
"""
import re, sys, argparse, statistics

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--from', dest='lo', type=int, default=0)
    ap.add_argument('--to', dest='hi', type=int, default=10**9)
    ap.add_argument('--thread', type=int, default=0, help='guest thread (vCPU) column that is the game thread')
    a = ap.parse_args()
    mark = re.compile(r'\[insn\] mark frame=(-?\d+) caller=(\d+)((?: t\d+=\d+)+)')
    names = {}
    ft = {}
    marks = []   # (frame_or_task, [counts per thread])
    for line in open(a.log, errors='replace'):
        m = mark.search(line)
        if m:
            v = int(m.group(1))
            counts = [int(x.split('=')[1]) for x in m.group(3).split()]
            marks.append((v, counts))
            continue
        m = re.match(r'\[taskmarks\] (\d+)=(.*)', line)
        if m:
            names[int(m.group(1))] = m.group(2).strip()
            continue
        m = re.match(r'\[ft\] (\d+) (\d+) (\d+) (-?\d+)', line)
        if m:
            ft[int(m.group(1))] = (int(m.group(2)), int(m.group(4)))
    T = a.thread
    frame = 0
    per_frame_total = {}                 # frame -> game thread instructions between this frame mark and the previous
    per_frame_other = {}
    task_in_frame = {}                   # (frame, idx) -> instructions
    open_task = {}                       # idx -> counts at begin
    last_frame_counts = None
    cur_frame_tasks = {}
    prev_counts = None
    for v, counts in marks:
        if v >= 0x20000000:
            idx = (v - 0x20000000) >> 1
            if (v & 1) == 0:
                open_task[idx] = counts
            else:
                b = open_task.pop(idx, None)
                if b is not None:
                    d = counts[T] - b[T] if len(counts) > T and len(b) > T else 0
                    cur_frame_tasks[idx] = cur_frame_tasks.get(idx, 0) + d
            continue
        if v < 0:
            continue
        # a frame mark: attribute everything since the previous frame mark to this frame
        if last_frame_counts is not None and a.lo <= v <= a.hi:
            tot = counts[T] - last_frame_counts[T]
            oth = sum(counts) - counts[T] - (sum(last_frame_counts) - last_frame_counts[T])
            per_frame_total[v] = tot
            per_frame_other[v] = oth
            for idx, d in cur_frame_tasks.items():
                task_in_frame[(v, idx)] = d
        last_frame_counts = counts
        cur_frame_tasks = {}
    frames = sorted(per_frame_total)
    if not frames:
        print('no frame marks in the window (run with MSC_QEMU_MARK=1 and the plugin)'); return 1
    def stats(xs):
        xs = sorted(xs)
        return (statistics.mean(xs), xs[len(xs) // 2], xs[min(len(xs) - 1, int(len(xs) * 0.95))], xs[-1])
    M = 1e6
    print('frames %d..%d (%d frames)   game thread = t%d' % (frames[0], frames[-1], len(frames), T))
    print('%-28s %7s %9s %9s %9s %9s' % ('task', 'frames', 'mean M', 'median M', 'p95 M', 'max M'))
    idxs = sorted({i for (_, i) in task_in_frame})
    sumtasks = 0
    for i in idxs:
        xs = [task_in_frame[(f, i)] for f in frames if (f, i) in task_in_frame]
        s = stats(xs)
        sumtasks += s[0]
        print('%-28s %7d %9.3f %9.3f %9.3f %9.3f' % (names.get(i, 'task %d' % i), len(xs), s[0] / M, s[1] / M, s[2] / M, s[3] / M))
    tot = stats([per_frame_total[f] for f in frames])
    oth = stats([per_frame_other[f] for f in frames])
    print('%-28s %7d %9.3f %9.3f %9.3f %9.3f' % ('(sum of the tasks, means)', len(frames), sumtasks / M, 0, 0, 0))
    print('%-28s %7d %9.3f %9.3f %9.3f %9.3f' % ('game thread, whole frame', len(frames), tot[0] / M, tot[1] / M, tot[2] / M, tot[3] / M))
    print('%-28s %7d %9.3f %9.3f %9.3f %9.3f' % ('other threads, whole frame', len(frames), oth[0] / M, oth[1] / M, oth[2] / M, oth[3] / M))
    if ft:
        by = {}
        prev = None
        for f in frames:
            if f in ft and (f - 1) in ft:
                steps = ft[f][0] - ft[f - 1][0]
                by.setdefault(min(max(steps, 0), 5), []).append(per_frame_total[f])
        if by:
            print('\ngame thread per frame by simulation steps in the frame (the frame is 1/60 s of game time at 60 presented):')
            for k in sorted(by):
                s = stats(by[k])
                print('  %d step(s): %4d frames, mean %.3f M, median %.3f M, p95 %.3f M' % (k, len(by[k]), s[0] / M, s[1] / M, s[2] / M))
    return 0

if __name__ == '__main__':
    sys.exit(main())
