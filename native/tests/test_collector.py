"""Host runtime tests for build/shift-deck (read-only OBD 01 0C / 01 0D collector)
and repository packaging/consistency invariants."""
import importlib.util
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import threading
import unittest

ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(__file__).resolve().parents[1] / "build"


def exact(conn, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise EOFError("short read")
        buf.extend(chunk)
    return bytes(buf)


def hsfz_frame(control, payload):
    return struct.pack(">IH", len(payload), control) + payload


class CollectorTest(unittest.TestCase):
    def run_worker(self, args, transactions, env=None, stream="stdout"):
        errors = []
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            listener.settimeout(3)

            def gateway():
                try:
                    conn, _ = listener.accept()
                    with conn:
                        conn.settimeout(3)
                        for request, responses in transactions:
                            size, control = struct.unpack(">IH", exact(conn, 6))
                            self.assertEqual(control, 1)
                            self.assertEqual(exact(conn, size), bytes.fromhex("f412" + request))
                            for response in responses:
                                conn.sendall(hsfz_frame(1, bytes.fromhex("12f4" + response)))
                        self.assertEqual(conn.recv(1), b"", "Unexpected repeat or extra command")
                except Exception as error:
                    errors.append(error)

            thread = threading.Thread(target=gateway)
            thread.start()
            result = subprocess.run(
                [BUILD / "shift-deck", "--host", "127.0.0.1", "--port", str(listener.getsockname()[1]), *args],
                capture_output=True, text=True, check=True, timeout=6, env=env)
            thread.join(4)
            self.assertFalse(thread.is_alive())
            if errors:
                raise errors[0]
            return getattr(result, stream)

    @staticmethod
    def closed_port():
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            return s.getsockname()[1]

    @staticmethod
    def most_env(**extra):
        env = dict(os.environ, ID6_FAKE_MOST="up")
        env.update(extra)
        return env

    def test_decodes_live_rpm_and_speed(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
            listener.bind(("127.0.0.1", 0))
            listener.settimeout(2)
            self.run_worker(["--cycles", "1", "--udp-port", str(listener.getsockname()[1])],
                            [("010c", ["410c61a8"]), ("010d", ["410d64"])])
            sampled_ms, rpm, kph = struct.unpack("=Qii", listener.recv(64))
            self.assertGreater(sampled_ms, 0)
            self.assertEqual((rpm, kph), (6250, 100))

    def test_waits_for_busy_gateway(self):
        with socket.socket() as gateway, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as lock:
            gateway.bind(("127.0.0.1", 0))
            gateway.listen()
            gateway.settimeout(0.2)
            lock.bind(("127.0.0.1", 42861))
            with subprocess.Popen(
                [BUILD / "shift-deck", "--host", "127.0.0.1",
                 "--port", str(gateway.getsockname()[1]), "--cycles", "6"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            ) as worker:
                try:
                    with self.assertRaises(TimeoutError):
                        gateway.accept()
                    self.assertIsNone(worker.poll())
                    lock.close()
                    gateway.settimeout(2)
                    conn, _ = gateway.accept()
                    with conn:
                        conn.settimeout(2)
                        length, ctrl = struct.unpack(">IH", exact(conn, 6))
                        self.assertEqual(ctrl, 1)
                        self.assertEqual(exact(conn, length), bytes.fromhex("f412010c"))
                finally:
                    if worker.poll() is None:
                        worker.terminate()

    def test_hsfz_to_most_fallback(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.bind(("127.0.0.1", 0))
            udp.settimeout(5)
            r = subprocess.run(
                [BUILD / "shift-deck", "--host", "127.0.0.1", "--port", str(self.closed_port()),
                 "--udp-port", str(udp.getsockname()[1]), "--cycles", "40"],
                capture_output=True, text=True, timeout=20, env=self.most_env())
            _, rpm, kph = struct.unpack("=Qii", udp.recv(64))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual((rpm, kph), ((0x1A * 256 + 0xF8) // 4, 60))
        self.assertIn("transport: MOST", r.stderr)

    def test_hsfz_never_binds_most(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
            listener.bind(("127.0.0.1", 0))
            listener.settimeout(2)
            err = self.run_worker(["--cycles", "1", "--udp-port", str(listener.getsockname()[1])],
                                  [("010c", ["410c61a8"]), ("010d", ["410d64"])],
                                  env=self.most_env(), stream="stderr")
        self.assertNotIn("FAKE-MOST bind", err)

    def test_transport_most_trace_and_sigterm(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.bind(("127.0.0.1", 0))
            udp.settimeout(5)
            proc = subprocess.Popen(
                [BUILD / "shift-deck", "--transport", "most", "--udp-port", str(udp.getsockname()[1])],
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                env=self.most_env(SHIFTDECK_TRACE="1"))
            try:
                _, rpm, kph = struct.unpack("=Qii", udp.recv(64))
                self.assertEqual((rpm, kph), ((0x1A * 256 + 0xF8) // 4, 60))
            finally:
                proc.send_signal(signal.SIGTERM)
                rc = proc.wait(5)
                err = proc.communicate()[1]
        self.assertEqual(rc, 0, err)
        self.assertIn("FAKE-MOST exit unregister_target=1 unregister_client=1 free=1", err)
        self.assertIn("[dsi", err)


class RepoInvariantsTest(unittest.TestCase):
    def test_polyform_license_disclosed_and_packaged(self):
        notice = ROOT / "third-party" / "nbtevo-carplay-advanced-NOTICE"
        self.assertTrue(notice.is_file(), f"missing {notice}")
        self.assertIn("PolyForm Noncommercial License 1.0.0", notice.read_text(encoding="utf-8"))
        readme = (ROOT / "README.md").read_text(encoding="utf-8")
        self.assertIn("PolyForm Noncommercial License 1.0.0", readme)
        self.assertIn("hud_rg", readme)
        pkg_sh = (ROOT / "native" / "package.sh").read_text(encoding="utf-8")
        self.assertIn("nbtevo-carplay-advanced-NOTICE", pkg_sh)

    def test_check_app_reserved_connectors_match_known_id6_apps(self):
        check_app_path = ROOT / ".claude" / "skills" / "extendedhud" / "check_app.py"
        spec = importlib.util.spec_from_file_location("check_app", check_app_path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        expected = {
            "100": "BoostDeck",
            "102": "EngineCodes",
            "103": "MhdMaps",
            "104": "ShiftDeck",
            "107": "HudTest",
            "111": "Bootmod3Suite",
            "112": "XDriveControl",
            "120": "ExhaustFlap",
        }
        for cid, label in expected.items():
            self.assertEqual(mod.RESERVED_CONNECTORS.get(cid), label, f"connector {cid}")
        self.assertNotIn("121", mod.RESERVED_CONNECTORS)
        deploy_skill = (ROOT / ".claude" / "skills" / "deploy-plugins" / "SKILL.md").read_text(encoding="utf-8")
        self.assertIn("111", deploy_skill)
        self.assertIn("112", deploy_skill)


if __name__ == "__main__":
    unittest.main()
