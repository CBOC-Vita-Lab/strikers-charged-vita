#!/usr/bin/env python3
"""vitalab.py: client of the lab mode built into our Vita application (vita/src/vita_lab.cpp).

The application (a LAB=1 build) waits on TCP port 1339 before the game starts. This client
uploads files, starts runs and fetches logs. Python 3.6+, standard library only.

  vitalab.py [--host H] [--port 1339] [--timeout 30] [--budget 140] <command> ...
  (host: --host, or the environment variable VITA_HOST = the address on the console's screen)

  ping                         one line: build, variant, title id, address, how the last run ended
  put <local> <vita path>      upload; RESUMABLE: an interrupted upload continues where it
                               stopped when the same command is given again (exit status 4 =
                               "not finished within the budget, run the same command again")
  get <vita path> <local>      download (CRC checked)
  ls [vita path]               list a folder (default: ux0:data/mscharged)
  stat <vita path>             type and size
  rm <vita path>               remove a file or an empty folder
  mkdir <vita path>            create a folder (and its parents)
  mv <from> <to>               rename / replace
  run [seconds]                start the game in the WAITING process (see "two ways" below)
  exec <file> [--run [seconds]]  start another executable of the application folder; with
                               --run it begins a run at once (no server phase)
  wait-back [timeout]          poll until the command server answers again (default: the budget)
  cycle <env.txt> <tag> [seconds] [--exec FILE] [--same-process] [--input FILE ...] [--keep]
                               the whole test run: upload env.txt (and input scripts), start
                               the run, wait until the application is back, fetch log.txt,
                               lab/state.txt, lab.txt and any prof*.bin / bench*.txt into the
                               folder <tag>/. RESTARTABLE: it keeps its progress in
                               <tag>/cycle.json; when the budget (140 s) is used up it stops
                               with exit status 4 and the SAME command line continues it.
  start <env.txt> <tag> [seconds] [...]   only the first part of cycle (returns after RUN)
  fetch <tag>                  only the last part of cycle (the application must be back)
  play                         leave lab mode: the game starts as for a player
  exit                         back to the console's home screen (the owner must start it again)
  reboot --yes                 cold reset of the console (see the README before using it)

Two ways to start a run. `cycle` and `start` use "EXEC <running executable> RUN": the
application reloads itself and the NEW process runs the game at once. That process read
env.txt at the ordinary place (before the C++ static initialisers, some of which read
switches) and never loaded the network module: it is what a player's start is. `run`, and
`cycle --same-process`, start the game in the process that served the commands: env.txt is
read again, but switches read by static initialisers keep the values of the previous env.txt
(MSC_FAST*, the early memory switches). Use it only if a reload does not work.

Vita paths: ux0:data/mscharged/... and ux0:app/<title id>/... (also app0:...). A path without
a device name is relative to ux0:data/mscharged/ ("log.txt", "lab/state.txt"). The server
refuses everything else.

Exit status: 0 done; 2 error (message on stderr); 3 the run ended without a proper report or
the reload failed; 4 not finished within the budget: give the same command again.
Every network operation has a timeout (--timeout: seconds without any progress); a transfer
that stalls is reported, never waited for for ever. One line is printed per step.
"""
import argparse
import json
import os
import socket
import sys
import time
import zlib

DATA = "ux0:data/mscharged"
CHUNK = 64 * 1024
T0 = time.time()


class LabError(Exception):
    pass


class NotFinished(Exception):
    """The budget of this invocation is used up; the same command continues the work."""


def say(text):
    print("[%4.0fs] %s" % (time.time() - T0, text))
    sys.stdout.flush()


def vpath(path):
    """A path without a device name is relative to the data folder."""
    if ":" in path:
        return path
    return DATA + "/" + path.lstrip("/") if path.strip("/") else DATA


def file_crc(path, limit=None):
    crc = 0
    left = limit
    with open(path, "rb") as f:
        while left is None or left > 0:
            piece = f.read(1 << 20 if left is None else min(1 << 20, left))
            if not piece:
                break
            crc = zlib.crc32(piece, crc)
            if left is not None:
                left -= len(piece)
    return crc & 0xFFFFFFFF


class Lab:
    def __init__(self, host, port, timeout, budget):
        self.host, self.port, self.timeout = host, port, timeout
        self.deadline = (T0 + budget) if budget > 0 else None
        self.sock = None
        self.buf = b""

    def left(self):
        """Seconds of budget left (a large number without a budget)."""
        return 1e9 if self.deadline is None else self.deadline - time.time()

    # -- connection ---------------------------------------------------------------------
    def connect(self, connect_timeout=None):
        self.close()
        try:
            self.sock = socket.create_connection((self.host, self.port), timeout=connect_timeout or min(self.timeout, 8))
        except OSError as e:
            raise LabError("cannot connect to %s:%d: %s" % (self.host, self.port, e))
        self.sock.settimeout(self.timeout)
        self.buf = b""

    def close(self):
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = None

    def _recv(self, what):
        try:
            data = self.sock.recv(CHUNK)
        except socket.timeout:
            raise LabError("STALLED: no data from the console for %g s while %s" % (self.sock.gettimeout(), what))
        except OSError as e:
            raise LabError("connection lost while %s: %s" % (what, e))
        if not data:
            raise LabError("the console closed the connection while %s" % what)
        return data

    def _send(self, data, what):
        try:
            self.sock.sendall(data)
        except socket.timeout:
            raise LabError("STALLED: the console took no data for %g s while %s" % (self.sock.gettimeout(), what))
        except OSError as e:
            raise LabError("connection lost while %s: %s" % (what, e))

    def line(self, what):
        while b"\n" not in self.buf:
            self.buf += self._recv(what)
            if len(self.buf) > 65536 and b"\n" not in self.buf:
                raise LabError("no line end in 64 KiB of reply while %s" % what)
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode("utf-8", "replace").rstrip("\r")

    def take(self, size, what, sink=None):
        """Read exactly `size` bytes of payload; returns them (or feeds `sink`) and the CRC."""
        crc = 0
        got = 0
        parts = []
        while got < size:
            if not self.buf:
                self.buf = self._recv("%s (%d of %d bytes)" % (what, got, size))
            piece, self.buf = self.buf[: size - got], self.buf[size - got:]
            crc = zlib.crc32(piece, crc)
            got += len(piece)
            if sink:
                sink.write(piece)
            else:
                parts.append(piece)
        return b"".join(parts), crc & 0xFFFFFFFF

    def command(self, text, what=None):
        """One command with a one-line answer. Returns the text after 'OK '."""
        if self.sock is None:
            self.connect()
        what = what or text.split(" ")[0]
        self._send(text.encode("utf-8") + b"\n", what)
        reply = self.line(what)
        if reply.startswith("OK"):
            return reply[2:].strip()
        if reply.startswith("ERR"):
            raise LabError("%s refused: %s" % (what, reply[3:].strip()))
        raise LabError("unexpected reply to %s: %r" % (what, reply))

    # -- commands -----------------------------------------------------------------------
    def ping(self, connect_timeout=None):
        self.connect(connect_timeout)
        try:
            return self.command("PING")
        finally:
            self.close()

    def put(self, local, remote):
        """Upload. Resumable against a server that knows PUTR; plain PUT otherwise."""
        remote = vpath(remote)
        size = os.path.getsize(local)
        crc_all = file_crc(local)
        t0 = time.time()
        offset = 0
        resumable = True
        for attempt in range(3):
            if self.sock is None:
                self.connect()
            try:
                head = self.command("PUTR %d %08X %s" % (size, crc_all, remote), "PUT (asking)").split()
            except LabError as e:
                if "unknown command" not in str(e):
                    raise
                resumable = False
                self.command("PUT %d %s" % (size, remote), "PUT (asking)")
                break
            # "send <size> from <offset> <crc of the bytes the console already has>"
            offset = int(head[3])
            if offset == 0 or (offset <= size and int(head[4], 16) == file_crc(local, offset)):
                break
            # what the console kept from an earlier attempt is not the beginning of this file
            self.close()
            self.connect()
            self.command("RM %s.part" % remote, "RM of the stale partial upload")
            say("put %s: the console's partial file (%d bytes) did not match; removed, starting again" % (remote, offset))
            offset = 0
        else:
            raise LabError("PUT %s: the console keeps offering a partial file that does not match" % remote)
        try:   # a small send buffer: progress lines and the budget then follow what the console has really taken
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 128 * 1024)
        except OSError:
            pass
        if offset:
            say("put %s: continuing at %d of %d bytes (%d %%)" % (remote, offset, size, offset * 100 // max(size, 1)))
        sent = offset
        last_line = time.time()
        with open(local, "rb") as f:
            f.seek(offset)
            while True:
                if resumable and self.left() < 8 and size - sent > 4 * CHUNK:
                    # stop cleanly: the console keeps what it has as <path>.part
                    self.close()
                    raise NotFinished("put %s: %d of %d bytes are on the console (%d %%)" % (remote, sent, size, sent * 100 // max(size, 1)))
                piece = f.read(CHUNK)
                if not piece:
                    break
                self._send(piece, "uploading %s (%d of %d bytes)" % (remote, sent, size))
                sent += len(piece)
                if time.time() - last_line >= 10:
                    last_line = time.time()
                    say("put %s: %d %% (%.1f of %.1f MiB, %.0f KiB/s)" % (remote, sent * 100 // max(size, 1), sent / 1048576.0, size / 1048576.0,
                                                                        (sent - offset) / 1024.0 / max(time.time() - t0, 1e-3)))
        # the console writes the last block, checks the sum and renames before it answers
        old = self.sock.gettimeout()
        self.sock.settimeout(max(5, min(max(old, 30), self.left())) if resumable else max(old, 60))
        try:
            reply = self.line("waiting for the console to confirm %s" % remote)
        except LabError as e:
            if resumable and "STALLED" in str(e):
                self.close()
                raise NotFinished("put %s: all bytes were sent but the console has not confirmed them yet" % remote)
            raise
        self.sock.settimeout(old)
        if not reply.startswith("OK"):
            raise LabError("PUT %s failed: %s" % (remote, reply[3:].strip() if reply.startswith("ERR") else reply))
        fields = reply.split()
        if int(fields[1]) != size or int(fields[2], 16) != crc_all:
            raise LabError("PUT %s: the console has %s bytes, crc %s; the local file has %d, crc %08X" % (remote, fields[1], fields[2], size, crc_all))
        dt = time.time() - t0
        say("put %s -> %s: %d bytes, crc32 %08X, %.1f s (%.0f KiB/s)%s" % (local, remote, size, crc_all, dt, (size - offset) / 1024.0 / max(dt, 1e-3),
                                                                          ", resumed at %d" % offset if offset else ""))

    def get(self, remote, local):
        remote = vpath(remote)
        t0 = time.time()
        size = int(self.command("GET %s" % remote, "GET").split()[0])
        tmp = local + ".part"
        with open(tmp, "wb") as f:
            _, crc = self.take(size, "downloading %s" % remote, sink=f)
        reply = self.line("waiting for the checksum of %s" % remote)
        if not reply.startswith("OK crc32 "):
            raise LabError("GET %s: bad trailer %r" % (remote, reply))
        if int(reply.split()[2], 16) != crc:
            raise LabError("GET %s: checksum mismatch (console %s, received %08X)" % (remote, reply.split()[2], crc))
        os.replace(tmp, local)
        say("get %s -> %s: %d bytes, crc32 %08X, %.1f s" % (remote, local, size, crc, time.time() - t0))
        return size

    def ls(self, remote):
        remote = vpath(remote)
        head = self.command("LS %s" % remote, "LS").split()
        text, _ = self.take(int(head[0]), "listing %s" % remote)
        entries = []
        for line in text.decode("utf-8", "replace").splitlines():
            kind, size, name = line.split(" ", 2)
            entries.append((kind, int(size), name))
        return entries, (len(head) > 1 and head[1] == "truncated")

    def wait_back(self, timeout, every=2.0):
        """Poll until PING answers. Returns the PING line; NotFinished when the time is up."""
        t0 = time.time()
        last_error = ""
        while True:
            try:
                return self.ping(connect_timeout=3)
            except LabError as e:
                last_error = str(e)
            if time.time() - t0 + every > timeout:
                raise NotFinished("the command server does not answer yet (%s)" % last_error)
            time.sleep(every)


def field(ping_line, name):
    """A field of the PING answer; 'last' runs to the end of the line."""
    key = " " + name + "="
    text = " " + ping_line
    i = text.find(key)
    if i < 0:
        return ""
    rest = text[i + len(key):]
    return rest if name == "last" else rest.split(" ", 1)[0]


def is_result(name):
    return (name.startswith("prof") and name.endswith(".bin")) or (name.startswith("bench") and name.endswith(".txt"))


# -- the test cycle: start, wait, fetch; its progress lives in <tag>/cycle.json --------------

def load_state(tag):
    try:
        with open(os.path.join(tag, "cycle.json")) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_state(tag, state):
    tmp = os.path.join(tag, "cycle.json.tmp")
    with open(tmp, "w") as f:
        json.dump(state, f)
    os.replace(tmp, os.path.join(tag, "cycle.json"))


def do_start(lab, a, state):
    tag = a.tag
    before = lab.ping()
    say("console: " + before)
    with open(os.path.join(tag, "ping-before.txt"), "w") as f:
        f.write(before + "\n")
    state.update(phase="starting", before=before, seconds=a.seconds)
    save_state(tag, state)
    lab.connect()
    if not a.keep:   # results of earlier runs would be taken for this one's
        entries, _ = lab.ls(DATA)
        for kind, _, name in entries:
            if kind == "f" and is_result(name):
                lab.command("RM %s/%s" % (DATA, name))
                say("removed old result %s" % name)
    lab.put(a.env, DATA + "/env.txt")
    for extra in a.input or []:
        lab.put(extra, DATA + "/" + os.path.basename(extra))
    with open(a.env, "rb") as src, open(os.path.join(tag, "env.txt"), "wb") as dst:
        dst.write(src.read())
    target = a.exec or (None if a.same_process else field(before, "self"))
    if target:
        reply = lab.command("EXEC %s RUN %d" % (target, a.seconds))
    else:
        reply = lab.command("RUN %d" % a.seconds)
    lab.close()
    state.update(phase="running", t_run=time.time(), how=reply)
    save_state(tag, state)
    say("started: " + reply)


def do_fetch(lab, tag, state):
    after = lab.ping()
    say("console: " + after)
    with open(os.path.join(tag, "ping-after.txt"), "w") as f:
        f.write(after + "\n")
    lab.connect()
    fetched = []
    for remote, local in (("log.txt", "log.txt"), ("lab/state.txt", "state.txt"), ("lab.txt", "lab.txt")):
        try:
            lab.get(DATA + "/" + remote, os.path.join(tag, local))
            fetched.append(local)
        except LabError as e:
            say("WARNING: %s" % e)
            lab.connect()
    entries, _ = lab.ls(DATA)
    for kind, _, name in entries:
        if kind == "f" and is_result(name):
            lab.get(DATA + "/" + name, os.path.join(tag, name))
            fetched.append(name)
    lab.close()
    last = field(after, "last")
    say("run: %s" % last)
    say("fetched into %s/: %s" % (tag, ", ".join(fetched)))
    ok = last.startswith("ended:") and "RELOAD-FAILED" not in last
    if state.get("before") and field(state["before"], "last") == last:
        say("WARNING: the console reports the same last run as before this one was started: this run did not end with a report")
        ok = False
    state.update(phase="done", after=after, ok=ok)
    save_state(tag, state)
    return 0 if ok else 3


def cmd_cycle(lab, a, parts):
    tag = a.tag
    os.makedirs(tag, exist_ok=True)
    state = load_state(tag)
    phase = state.get("phase")
    if "start" in parts:
        if phase == "done":
            say("%s/ already holds a finished cycle (%s); use another tag" % (tag, "ok" if state.get("ok") else "run ended badly"))
            return 0 if state.get("ok") else 3
        if phase == "starting":
            # an earlier invocation stopped between the uploads and RUN: if the server still
            # answers, nothing was started
            try:
                lab.ping(connect_timeout=3)
                phase = None
            except LabError:
                state.update(phase="running", t_run=os.path.getmtime(os.path.join(tag, "cycle.json")))
                save_state(tag, state)
                phase = "running"
        if phase is None:
            do_start(lab, a, state)
            if parts == ("start",):
                return 0
            time.sleep(min(3, max(lab.left() - 1, 0)))
    else:
        state = state or {"phase": "running", "t_run": time.time()}
    if "wait" in parts:
        t_run = state.get("t_run", time.time())
        try:
            lab.wait_back(max(lab.left() - 20, 4))
        except NotFinished as e:
            limit = state.get("seconds")
            age = time.time() - t_run
            if limit and age > limit + 150:
                raise NotFinished("OVERDUE: the run is %.0f s old and its limit was %d s: the application has not come back by itself "
                                  "(reload refused, or the system ended the process). Ask the owner to start it again; this command "
                                  "then fetches the log. %s" % (age, limit, e))
            raise NotFinished("the run is %.0f s old%s; %s" % (age, " (its limit: %d s, then the reload)" % limit if limit else "", e))
        say("the application is back, %.0f s after the start of the run" % (time.time() - t_run))
    return do_fetch(lab, tag, state)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=os.environ.get("VITA_HOST"))
    p.add_argument("--port", type=int, default=int(os.environ.get("VITA_PORT", "1339")))
    p.add_argument("--timeout", type=float, default=30.0, help="seconds without progress before a transfer counts as stalled")
    p.add_argument("--budget", type=float, default=140.0, help="seconds this invocation may take in all (0 = no limit); put, cycle and wait-back stop with status 4 and continue when repeated")
    sub = p.add_subparsers(dest="cmd")
    sub.add_parser("ping")
    s = sub.add_parser("put"); s.add_argument("local"); s.add_argument("remote")
    s = sub.add_parser("get"); s.add_argument("remote"); s.add_argument("local")
    s = sub.add_parser("ls"); s.add_argument("remote", nargs="?", default=DATA)
    s = sub.add_parser("stat"); s.add_argument("remote")
    s = sub.add_parser("rm"); s.add_argument("remote")
    s = sub.add_parser("mkdir"); s.add_argument("remote")
    s = sub.add_parser("mv"); s.add_argument("src"); s.add_argument("dst")
    s = sub.add_parser("uninstall"); s.add_argument("title", help="MSCP00001 MSCP00002 MSCH00001 CDXMS0002 MSCN00001 MSCN00002 MSCN00003 only (the server refuses others; never the running title)")
    s = sub.add_parser("run"); s.add_argument("seconds", nargs="?", type=int, default=900)
    s = sub.add_parser("exec"); s.add_argument("file"); s.add_argument("--run", nargs="?", type=int, const=900, default=None, metavar="SECONDS")
    s = sub.add_parser("wait-back"); s.add_argument("timeout", nargs="?", type=float, default=None)
    for name in ("cycle", "start"):
        s = sub.add_parser(name); s.add_argument("env"); s.add_argument("tag"); s.add_argument("seconds", nargs="?", type=int, default=900)
        s.add_argument("--exec", metavar="FILE", help="run this executable of the application folder instead of the one that is waiting")
        s.add_argument("--same-process", action="store_true", help="plain RUN in the waiting process (see the two ways above)")
        s.add_argument("--input", action="append", metavar="FILE", help="also upload this file into ux0:data/mscharged/ (an input script); may be repeated")
        s.add_argument("--keep", action="store_true", help="do not remove old prof*.bin / bench*.txt before the run")
    s = sub.add_parser("fetch"); s.add_argument("tag")
    sub.add_parser("play")
    sub.add_parser("exit")
    s = sub.add_parser("reboot"); s.add_argument("--yes", action="store_true")
    a = p.parse_args()
    if not a.cmd:
        p.error("no command (see --help)")
    if not a.host:
        p.error("no host: use --host or set VITA_HOST (the address on the console's waiting screen)")
    lab = Lab(a.host, a.port, a.timeout, a.budget)
    try:
        if a.cmd == "ping":
            print(lab.ping())
        elif a.cmd == "put":
            lab.put(a.local, a.remote)
        elif a.cmd == "get":
            lab.get(a.remote, a.local)
        elif a.cmd == "ls":
            entries, cut = lab.ls(a.remote)
            for kind, size, name in sorted(entries, key=lambda e: (e[0] != "d", e[2].lower())):
                print("%s %12d  %s" % (kind, size, name))
            if cut:
                print("(listing truncated by the server)")
        elif a.cmd == "stat":
            print(lab.command("STAT %s" % vpath(a.remote)))
        elif a.cmd == "rm":
            print(lab.command("RM %s" % vpath(a.remote)))
        elif a.cmd == "mkdir":
            print(lab.command("MKDIR %s" % vpath(a.remote)))
        elif a.cmd == "mv":
            print(lab.command("MV %s|%s" % (vpath(a.src), vpath(a.dst))))
        elif a.cmd == "uninstall":
            print(lab.command("UNINSTALL %s" % a.title))
        elif a.cmd == "run":
            print(lab.command("RUN %d" % a.seconds))
        elif a.cmd == "exec":
            print(lab.command("EXEC %s%s" % (a.file, "" if a.run is None else " RUN %d" % a.run)))
        elif a.cmd == "wait-back":
            t0 = time.time()
            reply = lab.wait_back(a.timeout if a.timeout is not None else max(lab.left() - 5, 4))
            say("back after %.0f s: %s" % (time.time() - t0, reply))
        elif a.cmd == "cycle":
            return cmd_cycle(lab, a, ("start", "wait", "fetch"))
        elif a.cmd == "start":
            return cmd_cycle(lab, a, ("start",))
        elif a.cmd == "fetch":
            os.makedirs(a.tag, exist_ok=True)
            return cmd_cycle(lab, a, ("fetch",))
        elif a.cmd == "play":
            print(lab.command("PLAY"))
        elif a.cmd == "exit":
            print(lab.command("EXIT"))
        elif a.cmd == "reboot":
            if not a.yes:
                p.error("reboot needs --yes (a cold reset: nothing of ours restarts by itself afterwards)")
            print(lab.command("REBOOT YES"))
    except NotFinished as e:
        say("NOT FINISHED: %s. Give the same command again." % e)
        return 4
    except LabError as e:
        sys.stdout.flush()
        sys.stderr.write("vitalab: %s\n" % e)
        return 2
    finally:
        lab.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
