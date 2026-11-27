"""The bad-network relay of the host pair rig (native/tools/mp/link_relay.py): what it forwards, what it drops, and that a run which went through nothing is INVALID.

Two plain UDP sockets stand for two consoles. Console i listens on REAL+i and is told that its partner lives at RELAY+j."""
import importlib.util
import json
import socket
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("link_relay", ROOT / "native/tools/mp/link_relay.py")
link_relay = importlib.util.module_from_spec(spec)
sys.modules["link_relay"] = link_relay
spec.loader.exec_module(link_relay)
pair_spec = importlib.util.spec_from_file_location("native_pair", ROOT / "native/tools/mp/native_pair.py")
native_pair = importlib.util.module_from_spec(pair_spec)
pair_spec.loader.exec_module(native_pair)

REAL, RELAY = 43000, 43100


def consoles():
    socks = []
    for i in range(2):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("127.0.0.1", REAL + i))
        s.settimeout(0.05)
        socks.append(s)
    return socks


class Run:
    def __init__(self, spec_text, seconds, trigger=None, seed=1):
        self.relay = link_relay.Relay(2, REAL, RELAY, link_relay.parse_spec(spec_text), seed, None, trigger)
        self.stop = time.time() + seconds
        self.thread = threading.Thread(target=self.relay.run, args=(self.stop,))

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *a):
        self.thread.join()
        for s in self.relay.socks:
            s.close()


def drain(sock, until):
    got = []
    while time.time() < until:
        try:
            got.append(sock.recvfrom(2048))
        except socket.timeout:
            pass
    return got


class RelayTests(unittest.TestCase):
    def test_forwards_with_the_partners_stand_in_as_source(self):
        c = consoles()
        try:
            with Run("lan", 1.0):
                c[0].sendto(b"hello", ("127.0.0.1", RELAY + 1))      # console 0 talks to console 1 through the relay
                got = drain(c[1], time.time() + 0.5)
                self.assertEqual([g[0] for g in got], [b"hello"])
                self.assertEqual(got[0][1][1], RELAY + 0)            # console 1 sees console 0 as RELAY+0
                c[1].sendto(b"back", got[0][1])                      # the reply goes to that source
                back = drain(c[0], time.time() + 0.4)
                self.assertEqual([g[0] for g in back], [b"back"])
                self.assertEqual(back[0][1][1], RELAY + 1)
        finally:
            for s in c:
                s.close()

    def test_loss_is_counted_and_a_full_loss_forwards_nothing(self):
        c = consoles()
        try:
            r = None
            with Run("lan,loss_pct=100", 1.0) as run:
                r = run.relay
                for i in range(20):
                    c[0].sendto(b"x%d" % i, ("127.0.0.1", RELAY + 1))
                got = drain(c[1], time.time() + 0.5)
            self.assertEqual(got, [])
            self.assertEqual(r.dirs[(0, 1)].stats["drop_loss"], 20)
            self.assertEqual(r.dirs[(0, 1)].stats["out"], 0)
        finally:
            for s in c:
                s.close()

    def test_outage_cuts_both_ways_then_ends(self):
        c = consoles()
        try:
            with Run("lan,outage_len=0.6,outage_at=0.0", 2.0) as run:
                time.sleep(0.1)
                c[0].sendto(b"during", ("127.0.0.1", RELAY + 1))
                c[1].sendto(b"during2", ("127.0.0.1", RELAY + 0))
                self.assertEqual(drain(c[1], time.time() + 0.3), [])
                self.assertEqual(drain(c[0], time.time() + 0.1), [])
                time.sleep(0.5)
                c[0].sendto(b"after", ("127.0.0.1", RELAY + 1))
                self.assertEqual([g[0] for g in drain(c[1], time.time() + 0.4)], [b"after"])
            st = run.relay.stats()
            self.assertEqual(st["0>1"]["drop_outage"], 1)
            self.assertEqual(st["1>0"]["drop_outage"], 1)
        finally:
            for s in c:
                s.close()

    def test_trigger_arms_the_outage_from_a_log_line(self):
        import re
        c = consoles()
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "peer.log"
            log.write_text("boot\n")
            trig = ([str(log)], re.compile(r"sync tick="), 0.0)
            try:
                with Run("lan,outage_len=0.5", 1.6, trigger=trig) as run:
                    time.sleep(0.3)
                    c[0].sendto(b"before", ("127.0.0.1", RELAY + 1))
                    self.assertEqual([g[0] for g in drain(c[1], time.time() + 0.2)], [b"before"])   # no trigger yet: forwarded
                    log.write_text("boot\n[mp-lan] sync tick=100 m=0\n")
                    time.sleep(0.2)
                    c[0].sendto(b"cut", ("127.0.0.1", RELAY + 1))
                    self.assertEqual(drain(c[1], time.time() + 0.2), [])
                self.assertEqual(run.relay.stats()["0>1"]["drop_outage"], 1)
            finally:
                for s in c:
                    s.close()

    def test_unknown_profile_is_refused(self):
        with self.assertRaises(ValueError):
            link_relay.parse_spec("nope")
        with self.assertRaises(ValueError):
            link_relay.parse_spec("lan,wrong=1")

    def test_a_run_through_a_relay_that_carried_nothing_is_invalid(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d)
            (out / "link-stats.json").write_text(json.dumps({"spec": "bad_wifi", "directions": {"0>1": {"out": 0, "in_": 0}, "1>0": {"out": 0, "in_": 0}}}))

            class Dead:
                def send_signal(self, s): pass
                def wait(self, t=None): return 0
            bad = native_pair.stop_relay(Dead(), out)
            self.assertTrue(bad and "forwarded no datagram" in bad[0])
            (out / "link-stats.json").write_text(json.dumps({"spec": "bad_wifi", "directions": {"0>1": {"out": 5, "in_": 5}, "1>0": {"out": 4, "in_": 4}}}))
            self.assertEqual(native_pair.stop_relay(Dead(), out), [])


if __name__ == "__main__":
    unittest.main()
