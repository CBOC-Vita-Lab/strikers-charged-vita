#!/usr/bin/env python3
"""W5 CTL: the per-line controls test as a table.   tools/ctl/lines_report.py <run log> [<run log> ...]

Reads the "[script] CHECK RESULT <name> <verdict> tries=<n> fails=<n> : <layout> | <input> | <context>: <text> -> <counter>" lines
the harness prints at exit (tools/ctl/nav/ctl_lines_*.txt) and prints one Markdown row per check, then the totals."""
import re
import sys

PAT = re.compile(r'\[script\] CHECK RESULT (\S+) (PASS|FAIL \(undecided\)|FAIL|NOT RUN) tries=(\d+) fails=(\d+) : (.*)$')


def main():
    for path in sys.argv[1:]:
        rows = []
        for line in open(path, errors='replace'):
            m = PAT.search(line.rstrip('\n'))
            if m:
                rows.append(m.groups())
        print('### %s' % path)
        print('| check | line | context: text -> counter | verdict | tries |')
        print('|---|---|---|---|---|')
        for name, verdict, tries, fails, desc in rows:
            parts = [p.strip() for p in desc.split('|')]
            line = parts[1] if len(parts) > 2 else ''
            rest = parts[2] if len(parts) > 2 else desc
            print('| %s | %s | %s | **%s** | %s |' % (name, line, rest, verdict, tries))
        n = {v: sum(1 for r in rows if r[1].startswith(v)) for v in ('PASS', 'FAIL', 'NOT RUN')}
        print('\n%d checks: %d passed, %d failed, %d not run\n' % (len(rows), n['PASS'], n['FAIL'], n['NOT RUN']))


if __name__ == '__main__':
    main()
