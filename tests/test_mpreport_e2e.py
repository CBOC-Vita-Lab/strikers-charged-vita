"""SEND A REPORT and DELETE MY ONLINE ACCOUNT, end to end on this machine: the game's own HTTP client and np_mpreport against the LOCAL fake service (server/fake/server.mjs,
the Worker's application code over in-memory SQLite on 127.0.0.1). Nothing leaves the machine. Skipped without a C++ compiler or node 22.5+."""
import json
import os
import shutil
import socket
import subprocess
import tempfile
import time
import unittest
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
N = ROOT / "native"
SRC = ["platform/sdk/tests/host_report_e2e.cpp", "platform/sdk/net/np_mpreport.cpp", "platform/sdk/net/np_mpreport_net.cpp", "platform/sdk/net/np_mpdiag.cpp",
       "platform/sdk/net/online/np_http.cpp", "platform/sdk/net/online/np_http_plain.cpp", "platform/sdk/net/online/np_net.cpp"]


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def node_ok():
    if not shutil.which("node"):
        return False
    v = subprocess.run(["node", "-v"], capture_output=True, text=True).stdout.strip().lstrip("v").split(".")
    return (int(v[0]), int(v[1])) >= (22, 5)


@unittest.skipUnless(shutil.which("c++") and node_ok() and shutil.which("openssl"), "needs c++, node 22.5+ and openssl")
class ReportEndToEnd(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.exe = Path(cls.tmp.name) / "e2e"
        c = subprocess.run(["c++", "-std=c++17", "-I", str(N), *[str(N / s) for s in SRC], "-pthread", "-o", str(cls.exe)], capture_output=True, text=True)
        if c.returncode:
            raise RuntimeError(c.stdout + c.stderr)
        cls.http, https = free_port(), free_port()
        cls.base = f"http://127.0.0.1:{cls.http}"
        cls.server = subprocess.Popen(["node", str(ROOT / "server/fake/server.mjs"), "--host", "127.0.0.1", "--http-port", str(cls.http), "--https-port", str(https)],
                                      cwd=str(ROOT / "server"), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cls.addClassCleanup(cls.server.terminate)
        for _ in range(100):
            try:
                urllib.request.urlopen(cls.base + "/__fake/status", timeout=1).read()
                break
            except Exception:
                time.sleep(0.1)
        else:
            raise RuntimeError("the fake service did not start")

    def call(self, method, path, body=None, token=None):
        req = urllib.request.Request(self.base + path, method=method, data=json.dumps(body).encode() if body is not None else None,
                                     headers={"content-type": "application/json", "x-msc-proto": "1", **({"authorization": "Bearer " + token} if token else {})})
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, json.loads(r.read() or b"{}")
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"{}")

    def register(self, name):
        s, d = self.call("POST", "/v1/register", {"name": name})
        self.assertEqual(s, 200)
        return d["token"]

    def run_e2e(self, mode, token, name, forget=None):
        env = dict(os.environ, E2E_URL=self.base, E2E_TOKEN=token, E2E_NAME=name)
        if forget:
            env["E2E_FORGET_FILE"] = str(forget)
        return subprocess.run([str(self.exe), mode], env=env, capture_output=True, text=True, timeout=60)

    def admin(self, path):
        req = urllib.request.Request(self.base + path, headers={"authorization": "Bearer fake-admin"})
        return json.loads(urllib.request.urlopen(req, timeout=5).read())

    def test_send_then_delete(self):
        token = self.register("E2eAlpha")
        forget = Path(self.tmp.name) / "identity"
        forget.write_text("x")
        r = self.run_e2e("send", token, "E2eAlpha", forget)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("Report sent. Thank you.", r.stdout)
        listing = self.admin("/v1/admin/reports")
        self.assertEqual(listing["total"], 1)
        one = self.admin("/v1/admin/reports/%d" % listing["reports"][0]["id"])
        text = json.dumps(one)
        self.assertIn("hello from the host check", text)
        self.assertNotIn("192.168.77.5", text)          # the address the driver logged was removed before sending
        self.assertNotIn(token, text)
        r = self.run_e2e("delete", token, "E2eAlpha", forget)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertFalse(forget.exists())                # this Vita forgot its identity
        self.assertEqual(self.admin("/v1/admin/reports")["total"], 0)   # and the service purged the report
        r = self.run_e2e("delete2", token, "E2eAlpha")   # already gone: still a success
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_three_a_day(self):
        token = self.register("E2eBravo")
        r = self.run_e2e("send4", token, "E2eBravo")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("You already sent 3 reports today. Try again tomorrow.", r.stdout)

    def test_no_connection_keeps_the_identity(self):
        forget = Path(self.tmp.name) / "identity2"
        forget.write_text("x")
        env = dict(os.environ, E2E_URL="http://127.0.0.1:%d" % free_port(), E2E_TOKEN="x", E2E_NAME="Z", E2E_FORGET_FILE=str(forget))
        r = subprocess.run([str(self.exe), "delete"], env=env, capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertTrue(forget.exists())
        r = subprocess.run([str(self.exe), "send"], env=env, capture_output=True, text=True, timeout=60)
        self.assertIn("No connection to the online service", r.stdout)


if __name__ == "__main__":
    unittest.main()
