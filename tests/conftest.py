"""Every skipped test is named in the summary (a test that cannot run here, for want of g++, Pillow, node or a git checkout, must not read as a pass)."""


def pytest_terminal_summary(terminalreporter):
    skipped = terminalreporter.stats.get("skipped", [])
    for rep in skipped:
        reason = rep.longrepr[2] if isinstance(rep.longrepr, tuple) and len(rep.longrepr) > 2 else str(rep.longrepr)
        terminalreporter.write_line(f"SKIPPED: {rep.nodeid}: {reason}")
    if skipped:
        terminalreporter.write_line(f"SKIPPED: {len(skipped)} test(s) did not run on this machine (above)")
