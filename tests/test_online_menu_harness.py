"""The online-menu gate must reject failed peers and observe real reconnect time."""
import contextlib
import itertools
import io
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time
import types
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
MP = ROOT / "native/tools/mp"
sys.path.insert(0, str(MP))
import native_pair
import online_menu_test as menu


def peer_text(index, restore):
    joined = "peer joined again" if index == 0 else "joined again"
    return ("[mp-lan] LAN mode on\n[mp-lan] link lost\n[mp-lan] connection lost\n"
            "[script] f=10 chose reconnect\n"), (
        "[np-net] rebuilding the link (generation 2)\n[online-menu] reconnecting\n"), (
        ("[online-menu] reconnected\n[script] f=20 STATE lobby restored\n"
         "[script] f=30 STATE " + joined + "\n[script] f=40 exit\n") if restore else
        ("[np-net] failed: The online connection could not be restored.\n"
         "[online-menu] lobby: reconnect failed: The online connection could not be restored.\n")), (
        "" if restore else "[script] f=1510 STATE reconnect failed popup\n"), (
        "" if restore else "[script] f=1540 STATE reconnect failure acknowledged\n"
        "[np-net] online session stopped\n"
        "[script] f=1550 STATE back in the main menu after the failed reconnect\n[script] f=1610 exit\n")


class Clock:
    now = 0.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class FakePair:
    """Only the external game/helper boundary is fake; analysis and observation are real."""
    def __init__(self, command, clock, helper_code=0, peer_codes=(0, 0), failure_at=46.0,
                 popup=True, duplicate=False, stop=True, script_exit=True):
        self.clock, self.started = clock, clock.now
        self.command = command
        self.code, self.peer_codes = helper_code, list(peer_codes)
        self.returncode = None
        self.out = Path(command[command.index("--out") + 1])
        self.restore = self.out.name == "restore"
        self.timeline = []
        for index in (0, 1):
            chunks = list(peer_text(index, self.restore))
            if not self.restore:
                if not popup:
                    chunks[2] = chunks[2].split("[online-menu]")[0]
                if duplicate:
                    chunks[2] += "[online-menu] lobby: reconnect failed: The connection could not be restored.\n"
                if not stop:
                    chunks[4] = chunks[4].replace("[np-net] online session stopped\n", "")
            if not script_exit:
                chunks = [chunk.replace("[script] f=40 exit\n", "").replace("[script] f=1610 exit\n", "") for chunk in chunks]
            times = (0.0, 1.0, 2.0, 2.1, 3.0) if self.restore else (0.0, 1.0, failure_at, failure_at + 0.1, max(51.0, failure_at + 1))
            self.timeline.append(list(zip(times, chunks)))
            (self.out / "pair" / ("peer%d" % index)).mkdir(parents=True)
        self.ends_at = max(t for events in self.timeline for t, _ in events)
        self.update()

    def update(self):
        elapsed = self.clock.now - self.started
        for index, events in enumerate(self.timeline):
            text = "".join(chunk for at, chunk in events if at <= elapsed)
            (self.out / "pair" / ("peer%d" % index) / "runtime.log").write_text(text)
        if elapsed >= self.ends_at:
            (self.out / "pair" / "codes.json").write_text(json.dumps(self.peer_codes))

    def poll(self):
        self.update()
        return self.code if self.clock.now - self.started >= self.ends_at else None

    def wait(self, timeout=None):
        self.clock.now = max(self.clock.now, self.started + self.ends_at)
        self.update()
        self.returncode = self.code
        return self.code

    def terminate(self):
        self.code = -15
        self.ends_at = 0

    kill = terminate


class LobbyAnalysis(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.out = Path(self.temp.name)
        self.logs = []
        for index in (0, 1):
            path = self.out / ("peer%d" % index) / "runtime.log"
            path.parent.mkdir()
            path.write_text("".join(peer_text(index, True)))
            self.logs.append(path)
        (self.out / "codes.json").write_text("[0, 0]")

    def test_default_game_analysis_still_requires_common_crc(self):
        self.assertFalse(native_pair.analyse(self.logs, [0, 0], "1")["passed"])

    def test_explicit_lobby_cli_accepts_clean_complete_lobby_without_crc(self):
        result = subprocess.run([sys.executable, str(MP / "native_pair.py"), "--binary", "unused",
                                 "--out", str(self.out), "--analyse-only", "--lobby-only", "--expect-lost", "1",
                                 "--end-marker0", "STATE peer joined again", "--end-marker1", "STATE joined again"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertEqual(json.loads(result.stdout)["analysis_mode"], "lobby-only")

    def lobby(self, codes=(0, 0), lost="1", endings=None):
        return native_pair.analyse(self.logs, list(codes), lost, lobby_only=True,
                                   end_markers=endings or ["STATE peer joined again", "STATE joined again"])

    def test_lobby_analysis_rejects_error_flags_and_wrong_loss_count(self):
        original = self.logs[0].read_text()
        errors = ("[mp-lan] DESYNC test", "[mp-lan] sync error", "[mp-lan] input queue overflow",
                  "[mp-lan] malformed", "[script] TIMEOUT", "SIGSEGV", "[fatal] failed",
                  "ERROR: AddressSanitizer: heap-use-after-free", "runtime error: invalid access")
        for error in errors:
            with self.subTest(error=error):
                self.logs[0].write_text(original + error + "\n")
                self.assertFalse(self.lobby()["passed"])
        self.logs[0].write_text(original)
        self.assertFalse(self.lobby(lost="0")["passed"])
        self.assertFalse(self.lobby(lost=None)["passed"])
        self.assertFalse(self.lobby(codes=(0, -11))["passed"])

    def test_lobby_requires_both_actual_end_markers_before_script_exit(self):
        self.assertFalse(self.lobby(endings=["missing", "STATE joined again"])["passed"])
        text = self.logs[0].read_text().replace("[script] f=40 exit\n", "")
        self.logs[0].write_text("[script] f=1 exit\n" + text)
        self.assertFalse(self.lobby()["passed"])

    def test_lobby_mode_still_rejects_crc_mismatch_when_present(self):
        for index, path in enumerate(self.logs):
            path.write_text(path.read_text() + "[mp-lan] sync tick=1 m=0 crc=%08x seed=12345678\n" % (index + 1))
        self.assertFalse(self.lobby()["passed"])

    def test_default_game_gate_still_accepts_matching_nonzero_crc(self):
        for path in self.logs:
            path.write_text(path.read_text() + "[mp-lan] sync tick=1 m=0 crc=12345678 seed=87654321\n")
        self.assertTrue(native_pair.analyse(self.logs, [0, 0], "1")["passed"])

    def test_default_game_gate_still_rejects_mismatching_common_crc(self):
        for index, path in enumerate(self.logs):
            path.write_text(path.read_text() + "[mp-lan] sync tick=1 m=0 crc=%08x seed=12345678\n" % (index + 1))
        self.assertFalse(native_pair.analyse(self.logs, [0, 0], "1")["passed"])

    def test_game_and_lobby_reject_placeholder_crc_values(self):
        originals = [path.read_text() for path in self.logs]
        for crc in ("00000000", "ffffffff"):
            for index, path in enumerate(self.logs):
                path.write_text(originals[index] + "[mp-lan] sync tick=1 m=0 crc=" + crc + " seed=12345678\n")
            with self.subTest(crc=crc, mode="game"):
                report = native_pair.analyse(self.logs, [0, 0], "1")
                self.assertEqual(report["sync_zero_crc_lines"], 2)
                self.assertFalse(report["passed"])
            with self.subTest(crc=crc, mode="lobby"):
                self.assertFalse(self.lobby()["passed"])

    def test_later_crc_line_cannot_hide_an_earlier_placeholder(self):
        for path in self.logs:
            path.write_text(path.read_text() +
                            "[mp-lan] sync tick=1 m=0 crc=00000000 seed=12345678\n"
                            "[mp-lan] sync tick=1 m=0 crc=12345678 seed=12345678\n")
        report = native_pair.analyse(self.logs, [0, 0], "1")
        self.assertEqual(report["sync_zero_crc_lines"], 2)
        self.assertFalse(report["passed"])

    def test_default_game_requires_actual_script_end_on_both_peers(self):
        originals = [path.read_text() + "[mp-lan] sync tick=1 m=0 crc=12345678 seed=87654321\n" for path in self.logs]
        for missing in ((0,), (1,), (0, 1)):
            with self.subTest(missing=missing):
                for index, path in enumerate(self.logs):
                    path.write_text(originals[index].replace("[script] f=40 exit\n", "") if index in missing else originals[index])
                self.assertFalse(native_pair.analyse(self.logs, [0, 0], "1")["passed"])

    def test_default_game_accepts_completed_stop_condition(self):
        for path in self.logs:
            path.write_text(path.read_text().replace("[script] f=40 exit\n", "") +
                            "[mp-lan] sync tick=1 m=0 crc=12345678 seed=87654321\n"
                            "[script] f=40 stopwhen: clock >= 62 (now 62.0012)\n"
                            "[plat] script stop condition at frame 40, exiting\n")
        report = native_pair.analyse(self.logs, [0, 0], "1")
        self.assertTrue(report["passed"])
        self.assertTrue(all(peer["script_stop"] and not peer["script_exit"] for peer in report["peers"]))

    def test_default_game_rejects_incomplete_or_unmatched_stop_condition(self):
        originals = [path.read_text().replace("[script] f=40 exit\n", "") +
                     "[mp-lan] sync tick=1 m=0 crc=12345678 seed=87654321\n" for path in self.logs]
        stop = "[script] f=40 stopwhen: clock >= 62 (now 62.0012)\n"
        end = "[plat] script stop condition at frame 40, exiting\n"
        for ending in (stop, end, stop + end.replace("40", "41"), end + stop,
                       "[plat] reached MSC_MAX_FRAMES=40 at frame 40, exiting\n"):
            with self.subTest(ending=ending):
                for index, path in enumerate(self.logs):
                    path.write_text(originals[index] + ending)
                self.assertFalse(native_pair.analyse(self.logs, [0, 0], "1")["passed"])

    def test_default_game_requires_actual_lan_activation_on_both_peers(self):
        originals = [path.read_text() + "[mp-lan] sync tick=1 m=0 crc=12345678 seed=87654321\n" for path in self.logs]
        for missing in ((0,), (1,), (0, 1)):
            with self.subTest(missing=missing):
                for index, path in enumerate(self.logs):
                    path.write_text(originals[index].replace("[mp-lan] LAN mode on\n", "") if index in missing else originals[index])
                self.assertFalse(native_pair.analyse(self.logs, [0, 0], "1")["passed"])

    def test_default_game_gate_rejects_fatal_or_sanitizer_with_clean_exit_and_crc(self):
        for path in self.logs:
            path.write_text(path.read_text() + "[mp-lan] sync tick=1 m=0 crc=12345678 seed=87654321\n")
        clean = self.logs[0].read_text()
        for error in ("[fatal] allocation failed", "ERROR: AddressSanitizer: invalid access", "runtime error: invalid access"):
            with self.subTest(error=error):
                self.logs[0].write_text(clean + error + "\n")
                self.assertFalse(native_pair.analyse(self.logs, [0, 0], "1")["passed"])

    def test_lobby_requires_both_peers_to_exit_cleanly(self):
        for codes in ((-11, 0), (0, -11), (0,), (0, "timeout(0)")):
            with self.subTest(codes=codes):
                self.assertFalse(self.lobby(codes=codes)["passed"])

    def test_lobby_cli_requires_explicit_loss_count_and_terminal_markers(self):
        result = subprocess.run([sys.executable, str(MP / "native_pair.py"), "--binary", "unused",
                                 "--out", str(self.out), "--analyse-only", "--lobby-only"],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires --expect-lost", result.stderr)


class DeterministicPairScene(unittest.TestCase):
    def pair_gate(self, watches):
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / "pair"
            for index, watch in enumerate(watches):
                log = out / "pair" / ("peer%d" % index) / "runtime.log"
                log.parent.mkdir(parents=True)
                log.write_text(
                    "[online-menu] multiplayer mode choice\n[script] f=10 chose online\n"
                    "[online-menu] online selected\n[online-menu] link up\n"
                    "[online-menu] account: name\n[mp-lan] online link up; entering the lobby\n"
                    "[mp-lan] LAN mode on\n[online-menu] chose " + ("host" if index == 0 else "join") + "\n"
                    "[kbd] typed\n[online-menu] invite code typed\n"
                    "signaling failed while a link is up\n[online-notice] banner: The match goes on.\n" + watch)
            args = types.SimpleNamespace(binary="unused", proxy="proxy", fake_server="unused", node="node",
                                         service_port=47000, sim_port=48000, port_base=41000, timeout=420, dump_every=100)
            results = []
            with mock.patch.object(menu.subprocess, "run", return_value=types.SimpleNamespace(returncode=0, stdout="", stderr="")), \
                    contextlib.redirect_stdout(io.StringIO()):
                menu.case_pair(args, out, "unused", results)
            return results

    @staticmethod
    def watch(teams="5v3", stadium=13, game=1):
        return "[watch] f=3000 teams=%s stad=%d game=%d gs=5\n" % (teams, stadium, game)

    def test_fixed_scene_preserves_all_old_checks_and_adds_both_peer_checks(self):
        results = self.pair_gate([self.watch(), self.watch()])
        self.assertEqual(len(results), 19)
        self.assertTrue(all(ok for _, ok, _ in results), results)

    def test_wrong_team_or_stadium_cannot_pass_the_pair_gate(self):
        for wrong in (self.watch("10v9"), self.watch(stadium=12)):
            for peer in (0, 1):
                with self.subTest(wrong=wrong, peer=peer):
                    watches = [self.watch(), self.watch()]
                    watches[peer] = wrong
                    results = self.pair_gate(watches)
                    self.assertTrue(any(not ok for _, ok, _ in results), results)

    def test_missing_or_only_pre_match_scene_is_not_scene_proof(self):
        for missing in ("", self.watch(game=0), "[watch] f=3000 game=1 gs=5\n"):
            with self.subTest(missing=missing):
                results = self.pair_gate([self.watch(), missing])
                self.assertTrue(any(not ok for _, ok, _ in results), results)

    def test_later_correct_scene_cannot_hide_an_earlier_different_scene(self):
        results = self.pair_gate([self.watch("10v9") + self.watch(), self.watch()])
        self.assertTrue(any(not ok for _, ok, _ in results), results)


class ObserverTiming(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.log = Path(self.temp.name) / "runtime.log"
        self.log.write_text("")
        self.clock = Clock()
        patch = mock.patch.object(menu.time, "monotonic", side_effect=self.clock.monotonic)
        patch.start()
        self.addCleanup(patch.stop)
        self.observer = menu.ReconnectObserver([self.log])

    def append(self, text, seconds=0.05):
        self.clock.now += seconds
        with self.log.open("a") as log:
            log.write(text)
        self.observer.poll()

    def start(self):
        self.append("[np-net] rebuilding the link (generation 2)\n[online-menu] reconnecting\n")

    def finish(self, popup_seconds=0.05):
        self.append("[np-net] failed: The online connection could not be restored.\n")
        self.append("[online-menu] lobby: reconnect failed: The online connection could not be restored.\n", popup_seconds)

    def test_partial_lines_are_not_timestamped_until_complete(self):
        self.append("[np-net] rebuilding the link (generation ")
        self.assertEqual(self.observer.peers[0]["events"], [])
        self.append("2)\n[online-menu] reconnecting\n")
        self.finish()
        self.assertTrue(self.observer.failure_within_window(0)[0])

    def test_observer_starvation_is_not_reported_as_a_timing_pass(self):
        self.start()
        self.append("", seconds=0.75)
        self.finish()
        ok, detail = self.observer.failure_within_window(0)
        self.assertFalse(ok)
        self.assertIn("sampling bound", detail)

    def test_terminal_event_is_required_not_only_a_popup_marker(self):
        self.start()
        self.append("[online-menu] lobby: reconnect failed: The connection could not be restored.\n")
        self.assertFalse(self.observer.failure_within_window(0)[0])

    def test_batched_lines_preserve_actual_event_order(self):
        start = "[np-net] rebuilding the link (generation 2)\n"
        events = {
            "choice": "[online-menu] reconnecting\n",
            "terminal": "[np-net] failed: The online connection could not be restored.\n",
            "popup": "[online-menu] lobby: reconnect failed: The connection could not be restored.\n",
        }
        for order in itertools.permutations(events):
            with self.subTest(order=order):
                self.log.write_text("")
                self.observer = menu.ReconnectObserver([self.log])
                self.append(start + "".join(events[name] for name in order))
                self.assertEqual(self.observer.failure_within_window(0)[0],
                                 order == ("choice", "terminal", "popup"))

    def test_early_relay_substate_does_not_hide_late_session_failure(self):
        self.start()
        self.append("[np-net] relay failed: An intermediate relay problem.\n"
                    "[np-net] relay path wait ended: The relay wait ended.\n")
        self.append("[online-menu] lobby: reconnect failed: The connection could not be restored.\n")
        for _ in range(940):
            self.append("")
        self.append("[np-net] failed: The online connection could not be restored.\n")
        self.assertFalse(self.observer.failure_within_window(0)[0])
        terminal = [e for e in self.observer.peers[0]["events"] if e["event"] == "terminal"]
        self.assertEqual(len(terminal), 1)
        self.assertTrue(terminal[0]["line"].startswith("[np-net] failed:"))

    def test_45_second_production_deadline_is_measured_independently(self):
        self.start()
        for _ in range(900):
            self.append("")
        self.finish()
        self.assertTrue(self.observer.failure_within_window(0)[0])

    def test_late_popup_is_rejected_even_after_timely_transport_failure(self):
        self.start()
        self.append("[np-net] failed: The online connection could not be restored.\n")
        for _ in range(60):
            self.append("")
        self.append("[online-menu] lobby: reconnect failed: The connection could not be restored.\n")
        self.assertFalse(self.observer.failure_within_window(0)[0])

    def test_slow_terminal_read_cannot_hide_a_late_completion_poll(self):
        self.start()
        for _ in range(899):
            self.append("")
        with self.log.open("a") as log:
            log.write("[np-net] failed: The online connection could not be restored.\n"
                      "[online-menu] lobby: reconnect failed: The connection could not be restored.\n")
        original_open = Path.open
        def slow_open(path, *args, **kwargs):
            self.clock.now += 15.0
            return original_open(path, *args, **kwargs)
        self.clock.now += 0.05
        with mock.patch.object(Path, "open", autospec=True, side_effect=slow_open):
            self.observer.poll()
        self.observer.poll()
        ok, detail = self.observer.failure_within_window(0)
        self.assertFalse(ok)
        self.assertIn("sampling bound", detail)

    def test_previous_slow_poll_is_counted_when_a_start_is_first_discovered(self):
        other = self.log.with_name("other.log")
        other.write_text("")
        self.observer = menu.ReconnectObserver([self.log, other])
        original_open = Path.open
        def slow_other(path, *args, **kwargs):
            if path == other:
                self.clock.now += 15.0
                with original_open(self.log, "a") as log:
                    log.write("[np-net] rebuilding the link (generation 2)\n[online-menu] reconnecting\n")
            return original_open(path, *args, **kwargs)
        with mock.patch.object(Path, "open", autospec=True, side_effect=slow_other):
            self.observer.poll()
        self.append("")
        self.finish()
        ok, detail = self.observer.failure_within_window(0)
        self.assertFalse(ok)
        self.assertIn("sampling bound", detail)


class LostMenuGate(unittest.TestCase):
    def run_case(self, **options):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        clock = Clock()
        processes = []
        def spawn(command, **kwargs):
            proc = FakePair(command, clock, **options)
            proc.pid = 10000 + len(processes)
            processes.append(proc)
            return proc
        def observe_exit(kind, pid, flags):
            if pid == os.getpid():
                raise ChildProcessError()
            proc = next(proc for proc in processes if proc.pid == pid)
            return types.SimpleNamespace(si_pid=pid) if proc.poll() is not None else None
        def run(command, **kwargs):
            proc = spawn(command)
            proc.wait()
            return types.SimpleNamespace(returncode=proc.code, stdout="", stderr="")
        args = types.SimpleNamespace(binary="unused", fake_server="unused", node="node", proxy="proxy",
                                     service_port=47000, sim_port=48000, port_base=41000, timeout=120, dump_every=0)
        results = []
        with mock.patch.object(menu.subprocess, "run", side_effect=run), \
                mock.patch.object(menu.subprocess, "Popen", side_effect=spawn), \
                mock.patch.object(menu.os, "waitid", side_effect=observe_exit), \
                mock.patch.object(menu.time, "monotonic", side_effect=clock.monotonic), \
                mock.patch.object(menu.time, "sleep", side_effect=clock.sleep), \
                contextlib.redirect_stdout(io.StringIO()):
            menu.case_lost(args, Path(temp.name) / "lost", "unused", results)
        return results, processes

    def test_lost_cases_reject_nonzero_helper_exit_despite_success_markers(self):
        results, _ = self.run_case(helper_code=7)
        self.assertTrue(any(not ok for _, ok, _ in results), results)

    def test_lost_cases_reject_nonzero_peer_exit_despite_success_markers(self):
        results, _ = self.run_case(peer_codes=(0, -11))
        self.assertTrue(any(not ok for _, ok, _ in results), results)

    def test_failure_popup_order_cannot_substitute_for_monotonic_deadline(self):
        results, _ = self.run_case(failure_at=60.0)
        self.assertTrue(any(not ok and "window" in name for name, ok, _ in results), results)

    def test_clean_lobby_and_actual_popup_pass(self):
        results, processes = self.run_case()
        self.assertTrue(all(ok for _, ok, _ in results), results)
        self.assertEqual(len(processes), 2)
        for proc in processes:
            self.assertIn("MSC_FPS=30", proc.command)
            self.assertIn("--lobby-only", proc.command)
            self.assertEqual(proc.command[proc.command.index("--expect-lost") + 1], "1")
            self.assertFalse(any("REBUILD" in arg or "RELAY_WAIT" in arg for arg in proc.command))

    def test_restored_scripts_hold_both_peers_equally_before_exit(self):
        holds = []
        for side in ("host", "join"):
            text = (MP / "lan" / ("online_menu_%s_lost.txt" % side)).read_text()
            holds.append(re.search(r"idle (\d+)\s+exit\s*$", text).group(1))
        self.assertEqual(holds, ["300", "300"])

    def test_script_exit_marker_is_required(self):
        results, _ = self.run_case(script_exit=False)
        self.assertTrue(any(not ok for _, ok, _ in results), results)

    def test_actual_popup_open_log_is_required(self):
        results, _ = self.run_case(popup=False)
        self.assertTrue(any(not ok for _, ok, _ in results), results)

    def test_exactly_one_failure_sentence_is_required(self):
        results, _ = self.run_case(duplicate=True)
        self.assertTrue(any(not ok and "sentence" in name for name, ok, _ in results), results)

    def test_main_menu_marker_does_not_prove_transport_stopped(self):
        results, _ = self.run_case(stop=False)
        self.assertTrue(any(not ok and "no link" in name for name, ok, _ in results), results)


class LostPairCleanup(unittest.TestCase):
    def test_cleanup_diagnostic_without_add_note_preserves_original_and_is_visible(self):
        class LegacyObserverError(Exception):
            add_note = None
        with tempfile.TemporaryDirectory() as temp:
            args = types.SimpleNamespace(binary="unused", fake_server="unused", node="node", proxy="proxy",
                                         service_port=47000, sim_port=48000, port_base=41000, timeout=120, dump_every=0)
            proc = mock.Mock(pid=123, returncode=None)
            original = LegacyObserverError("observer failed")
            diagnostic = io.StringIO()
            caught = None
            with mock.patch.object(menu.subprocess, "Popen", return_value=proc), \
                    mock.patch.object(menu.ReconnectObserver, "poll", side_effect=original), \
                    mock.patch.object(menu.os, "killpg", side_effect=PermissionError("cannot signal owned group")), \
                    contextlib.redirect_stderr(diagnostic):
                try:
                    menu.lost_pair(args, Path(temp) / "lost", "host.txt", "join.txt", False, 40)
                except BaseException as error:
                    caught = error
            self.assertIs(caught, original)
            self.assertIn("cleanup incomplete", diagnostic.getvalue())
            self.assertIn("PermissionError", diagnostic.getvalue())
            proc.wait.assert_called_once_with(5)

    def test_cleanup_errors_keep_original_failure_primary_and_attempt_bounded_reap(self):
        for stage, signal_error, wait_error in (
            ("signal", PermissionError("cannot signal owned group"), None),
            ("reap", ProcessLookupError("group exited"), subprocess.TimeoutExpired("helper", 5)),
        ):
            with self.subTest(stage=stage), tempfile.TemporaryDirectory() as temp:
                args = types.SimpleNamespace(binary="unused", fake_server="unused", node="node", proxy="proxy",
                                             service_port=47000, sim_port=48000, port_base=41000, timeout=120, dump_every=0)
                proc = mock.Mock(pid=123, returncode=None)
                proc.wait.side_effect = wait_error
                original = ValueError("observer failed")
                caught = None
                with mock.patch.object(menu.subprocess, "Popen", return_value=proc), \
                        mock.patch.object(menu.ReconnectObserver, "poll", side_effect=original), \
                        mock.patch.object(menu.os, "killpg", side_effect=signal_error):
                    try:
                        menu.lost_pair(args, Path(temp) / "lost", "host.txt", "join.txt", False, 40)
                    except BaseException as error:
                        caught = error
                self.assertIs(caught, original)
                proc.wait.assert_called_once_with(5)
                notes = "\n".join(getattr(original, "__notes__", []))
                self.assertIn("cleanup incomplete", notes)
                self.assertIn(type(signal_error if stage == "signal" else wait_error).__name__, notes)

    def test_interrupted_or_lost_waitability_never_signals_an_uncertain_group(self):
        for original in (ChildProcessError("waitability lost"), KeyboardInterrupt("wait observation interrupted")):
            with self.subTest(error=type(original).__name__), tempfile.TemporaryDirectory() as temp:
                args = types.SimpleNamespace(binary="unused", fake_server="unused", node="node", proxy="proxy",
                                             service_port=47000, sim_port=48000, port_base=41000, timeout=120, dump_every=0)
                proc = mock.Mock(pid=123, returncode=None)
                def observe_exit(kind, pid, flags):
                    if pid == os.getpid():
                        raise ChildProcessError()
                    raise original
                with mock.patch.object(menu.subprocess, "Popen", return_value=proc), \
                        mock.patch.object(menu.ReconnectObserver, "poll"), \
                        mock.patch.object(menu.os, "waitid", side_effect=observe_exit), \
                        mock.patch.object(menu.os, "killpg", side_effect=ProcessLookupError()) as killpg:
                    with self.assertRaises(type(original)) as caught:
                        menu.lost_pair(args, Path(temp) / "lost", "host.txt", "join.txt", False, 40)
                self.assertIs(caught.exception, original)
                killpg.assert_not_called()
                proc.wait.assert_called_once_with(5)

    def test_nonreaping_primitive_is_required_before_spawning(self):
        with tempfile.TemporaryDirectory() as temp:
            args = types.SimpleNamespace(binary="unused", fake_server="unused", node="node", proxy="proxy",
                                         service_port=47000, sim_port=48000, port_base=41000, timeout=120, dump_every=0)
            with mock.patch.object(menu.os, "waitid", None), \
                    mock.patch.object(menu.subprocess, "Popen") as spawn:
                with self.assertRaisesRegex(RuntimeError, "nonreaping"):
                    menu.lost_pair(args, Path(temp) / "lost", "host.txt", "join.txt", False, 40)
            spawn.assert_not_called()

    def test_reaped_helper_never_signals_a_possibly_reused_group(self):
        with tempfile.TemporaryDirectory() as temp:
            args = types.SimpleNamespace(binary="unused", fake_server="unused", node="node", proxy="proxy",
                                         service_port=47000, sim_port=48000, port_base=41000, timeout=120, dump_every=0)
            proc = mock.Mock(pid=123, returncode=0)
            original = KeyboardInterrupt("cancelled after helper was reaped")
            with mock.patch.object(menu.subprocess, "Popen", return_value=proc), \
                    mock.patch.object(menu.ReconnectObserver, "poll", side_effect=original), \
                    mock.patch.object(menu.os, "killpg") as killpg:
                with self.assertRaises(KeyboardInterrupt) as caught:
                    menu.lost_pair(args, Path(temp) / "lost", "host.txt", "join.txt", False, 40)
            self.assertIs(caught.exception, original)
            killpg.assert_not_called()
            proc.wait.assert_called_once_with(5)

    def test_exception_cleanup_stops_a_ready_term_ignoring_descendant(self):
        helper = r'''
import os, signal, sys, time
from pathlib import Path
if os.fork() == 0:
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    Path(sys.argv[1]).write_text(str(os.getpid()))
while True:
    time.sleep(1)
'''
        real_popen = subprocess.Popen
        proc = None
        child = None
        with tempfile.TemporaryDirectory() as temp:
            ready = Path(temp) / "owned-child-ready"
            args = types.SimpleNamespace(binary="unused", fake_server="unused", node="node", proxy="proxy",
                                         service_port=47000, sim_port=48000, port_base=41000, timeout=120, dump_every=0)
            def launch(command, **kwargs):
                nonlocal proc, child
                proc = real_popen([sys.executable, "-B", "-c", helper, str(ready)], **kwargs)
                deadline = time.monotonic() + 5
                while not ready.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(ready.exists(), "descendant did not install its TERM handler")
                child = int(ready.read_text())
                return proc
            def exists(pid):
                try:
                    os.kill(pid, 0)
                    return True
                except ProcessLookupError:
                    return False
            try:
                original = PermissionError("observer cannot read runtime log")
                with mock.patch.object(menu.subprocess, "Popen", side_effect=launch), \
                        mock.patch.object(menu.ReconnectObserver, "poll", side_effect=original):
                    with self.assertRaises(PermissionError) as caught:
                        menu.lost_pair(args, Path(temp) / "lost", "host.txt", "join.txt", False, 40)
                self.assertIs(caught.exception, original)
                self.assertIsNotNone(proc.returncode, "helper was not reaped")
                deadline = time.monotonic() + 3
                while exists(child) and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertFalse(exists(child), "TERM-ignoring descendant survived helper cleanup")
            finally:
                # Keep failed-test cleanup confined to the still-pinned session or the identified fixture child.
                if proc is not None and proc.returncode is None:
                    try:
                        os.killpg(proc.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    proc.wait(5)
                if child is None and ready.exists():
                    child = int(ready.read_text())
                if child is not None and exists(child):
                    status = subprocess.run(["ps", "-p", str(child), "-o", "pgid=", "-o", "command="],
                                            capture_output=True, text=True)
                    fields = status.stdout.strip().split(None, 1)
                    if len(fields) == 2 and fields[0] == str(proc.pid) and str(ready) in fields[1]:
                        try:
                            os.kill(child, signal.SIGKILL)
                        except ProcessLookupError:
                            pass

    def test_missing_owned_group_preserves_observer_error_and_reaps_helper(self):
        for stage in ("term", "kill"):
            with self.subTest(stage=stage), tempfile.TemporaryDirectory() as temp:
                args = types.SimpleNamespace(binary="unused", fake_server="unused", node="node", proxy="proxy",
                                             service_port=47000, sim_port=48000, port_base=41000, timeout=120, dump_every=0)
                proc = mock.Mock(pid=123, returncode=None)
                clock = Clock()
                def signal_group(pid, sig):
                    if stage == "term" or sig == signal.SIGKILL:
                        raise ProcessLookupError("group exited")
                original = PermissionError("observer cannot read runtime log")
                caught = None
                with mock.patch.object(menu.subprocess, "Popen", return_value=proc), \
                        mock.patch.object(menu.ReconnectObserver, "poll", side_effect=original), \
                        mock.patch.object(menu.os, "killpg", side_effect=signal_group) as killpg, \
                        mock.patch.object(menu.time, "monotonic", side_effect=clock.monotonic), \
                        mock.patch.object(menu.time, "sleep", side_effect=clock.sleep):
                    try:
                        menu.lost_pair(args, Path(temp) / "lost", "host.txt", "join.txt", False, 40)
                    except BaseException as error:
                        caught = error
                self.assertIs(caught, original)
                self.assertEqual(killpg.call_args_list[0], mock.call(123, signal.SIGTERM))
                if stage == "kill":
                    self.assertEqual(killpg.call_args_list[-1], mock.call(123, signal.SIGKILL))
                    self.assertTrue(all(call == mock.call(123, 0) for call in killpg.call_args_list[1:-1]))
                    self.assertEqual(clock.now, 5.0)
                else:
                    self.assertEqual(killpg.call_args_list, [mock.call(123, signal.SIGTERM), mock.call(123, 0)])
                proc.wait.assert_called_once_with(5)


class ProxyBuildTests(unittest.TestCase):
    """Online audit (1.3): ensure_proxy compiled the NAT simulator without turnsim.cpp / np_hash.cpp, the link failed, and the pair, cancel
    and lost cases were silently reported as SKIP ("no NAT simulator") whenever --proxy was not given."""

    @unittest.skipUnless(any(map(menu.shutil.which, ("clang++", "g++"))), "needs a C++ compiler")
    def test_the_harness_builds_its_own_nat_simulator(self):
        with tempfile.TemporaryDirectory() as out:
            exe = menu.ensure_proxy(types.SimpleNamespace(proxy=""), Path(out))
            self.assertTrue(exe and Path(exe).is_file(), "online_menu_test.ensure_proxy could not build natsim_proxy")


if __name__ == "__main__":
    unittest.main()
