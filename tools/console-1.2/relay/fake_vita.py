"""A fake VitaCompanion for the relay tests: FTP (own minimal server, standard library) and the command socket, both on 127.0.0.1 in a temp dir.
Nothing here ever leaves this machine. Paths "ux0:data/x" and "ux0:/data/x" both map to <root>/ux0/data/x."""
import os, socket, threading, time
from pathlib import Path


class FakeVita:
    def __init__(self, root):
        self.root = Path(root)
        self.fs = self.root / "ux0"
        self.ftp_log, self.cmd_log = [], []
        self.fail = {}            # {(verb, substring): n} -> answer 426 n times
        self.launch_mode = "normal"   # normal | crash | nolaunch
        self.launch_delay = 0.1
        self.running = False
        self._socks = []
        self.ftp_srv = self._listen(self._ftp_conn)
        self.cmd_srv = self._listen(self._cmd_conn)
        self.ftp_port, self.cmd_port = self.ftp_srv.getsockname()[1], self.cmd_srv.getsockname()[1]

    def _listen(self, handler):
        s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("127.0.0.1", 0)); s.listen(8); self._socks.append(s)
        def loop():
            while True:
                try: c, _ = s.accept()
                except OSError: return
                threading.Thread(target=handler, args=(c,), daemon=True).start()
        threading.Thread(target=loop, daemon=True).start()
        return s

    def close(self):
        for s in self._socks:
            try: s.close()
            except OSError: pass

    def local(self, p):
        p = p.strip()
        if p.startswith("ux0:"): p = p[4:]
        p = p.lstrip("/")
        return self.fs / p

    def put_file(self, path, data):
        f = self.local(path); f.parent.mkdir(parents=True, exist_ok=True); f.write_bytes(data)

    def read_file(self, path):
        f = self.local(path); return f.read_bytes() if f.is_file() else None

    def touched(self):
        return [l for l in self.ftp_log if l.split(" ", 1)[0] in ("STOR", "DELE", "RETR", "LIST", "MKD", "RNFR")]

    # ---- FTP
    def _ftp_conn(self, c):
        f = c.makefile("rwb", buffering=0)
        def say(t): f.write((t + "\r\n").encode())
        say("220 fake vitacompanion")
        pasv = None
        try:
            while True:
                line = f.readline()
                if not line: break
                line = line.decode().strip(); verb, _, arg = line.partition(" "); verb = verb.upper()
                self.ftp_log.append(f"{verb} {arg}".strip())
                inj = [k for k in self.fail if k[0] == verb and k[1] in arg and self.fail[k] > 0]
                if inj and verb in ("RETR", "STOR", "LIST"):
                    self.fail[inj[0]] -= 1
                    if pasv: pasv.close(); pasv = None
                    say("426 injected failure"); continue
                if verb == "USER": say("331 ok")
                elif verb == "PASS": say("230 ok")
                elif verb in ("TYPE", "NOOP", "MODE", "STRU"): say("200 ok")
                elif verb == "PWD": say('257 "/"')
                elif verb == "PASV":
                    pasv = socket.socket(); pasv.bind(("127.0.0.1", 0)); pasv.listen(1)
                    port = pasv.getsockname()[1]
                    say(f"227 Entering Passive Mode (127,0,0,1,{port >> 8},{port & 255})")
                elif verb in ("RETR", "LIST", "STOR"):
                    p = self.local(arg)
                    if verb == "RETR" and not p.is_file(): say("550 no such file"); (pasv.close() if pasv else None); pasv = None; continue
                    if verb == "LIST" and not p.is_dir(): say("550 no such folder"); (pasv.close() if pasv else None); pasv = None; continue
                    say("150 go"); d, _ = pasv.accept(); pasv.close(); pasv = None
                    if verb == "RETR": d.sendall(p.read_bytes())
                    elif verb == "LIST":
                        rows = []
                        for e in sorted(p.iterdir()):
                            st = e.stat()
                            rows.append(f"{'d' if e.is_dir() else '-'}rwxr-xr-x 1 vita vita {st.st_size} Jan  1 00:00 {e.name}")
                        d.sendall(("\r\n".join(rows) + ("\r\n" if rows else "")).encode())
                    else:
                        buf = b""
                        while True:
                            x = d.recv(65536)
                            if not x: break
                            buf += x
                        p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(buf)
                    d.close(); say("226 done")
                elif verb == "DELE":
                    p = self.local(arg)
                    if p.is_file(): p.unlink(); say("250 deleted")
                    else: say("550 no such file")
                elif verb == "QUIT": say("221 bye"); break
                else: say("502 not implemented")
        except OSError:
            pass
        finally:
            c.close()

    # ---- command port
    def _cmd_conn(self, c):
        try:
            c.settimeout(5)
            line = c.recv(4096).decode().strip()
            self.cmd_log.append(line)
            if line == "version": c.sendall(b"fake vitacompanion 1.0\r\n")
            elif line == "quit CDXMP0001": self.running = False; c.sendall(b"ok\r\n")
            elif line == "launch CDXMP0001":
                c.sendall(b"ok\r\n"); self._launch()
            else: c.sendall(b"ok\r\n")
        except OSError:
            pass
        finally:
            c.close()

    def _launch(self):
        self.running = True
        mode = self.launch_mode
        if mode == "nolaunch": return
        envp = self.local("ux0:data/mscharged-perf-test/np-player.txt")
        env = envp.read_text() if envp.is_file() else ""
        logp = self.local("ux0:data/mscharged-perf-test/log.txt")
        def work():
            time.sleep(self.launch_delay)
            logp.parent.mkdir(parents=True, exist_ok=True)
            head = "old line\n===== fake build\n" + "".join(f"[env] {l}\n" for l in env.splitlines() if l and not l.startswith("#"))
            logp.write_text(head + "[frame-timing] reason=window n=300 total_us=5000000 p50_us=16600 p95_us=20000 p99_us=30000 scene=play\n")
            if mode == "normal":
                time.sleep(self.launch_delay)
                logp.write_text(head + "[frame-timing] reason=window n=300 total_us=5000000 p50_us=16600 p95_us=20000 p99_us=30000 scene=play\n[script] script exit\n")
        threading.Thread(target=work, daemon=True).start()
