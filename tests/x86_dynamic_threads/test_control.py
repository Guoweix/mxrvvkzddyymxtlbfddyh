#!/usr/bin/env python3
"""Exercise allocation/cleanup with real processes and SOCK_SEQPACKET messages."""
import contextlib
import multiprocessing as mp
import os
from pathlib import Path
import select
import signal
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import unittest

HERE = Path(__file__).resolve().parent
BUILD = HERE.parent / "build"
WIRE = struct.Struct("=8I4Q12I")
MAGIC, VERSION = 0x44544752, 4


def actor(path, pipe):
    conn = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    conn.settimeout(5)
    conn.connect(path)
    session = slot = epoch = request = 0
    persistent = False

    def send(op, workers=0, maximum=8, epoch_override=None, tids=None,
             phase=0, prompt=0, decode=0, mask=0):
        nonlocal request, epoch
        request += 1
        if op == 3 and not persistent:
            epoch += 1
        tids = tids or []
        if workers == 1 and op == 4 and not tids:
            import os
            tids = [os.getpid()]
        values = [MAGIC, VERSION, op, 0, slot, workers, maximum, 0,
                  request, session, epoch if epoch_override is None else epoch_override, mask] + tids + [0] * (8-len(tids))
        conn.sendall(WIRE.pack(*values, 0, phase, prompt, decode))

    def receive():
        nonlocal epoch
        result = WIRE.unpack(conn.recv(WIRE.size))
        if result[:2] != (MAGIC, VERSION) or result[8] not in (0, request):
            raise RuntimeError("bad response identity")
        if result[2] == 7:
            epoch = result[10]
        return {"op": result[2], "status": result[3], "slot": result[4], "workers": result[5],
                "pin": result[7], "session": result[9], "epoch": result[10], "mask": result[11],
                "allocation": result[20], "phase": result[21]}

    try:
        send(1)
        msg = receive()
        session, slot = msg["session"], msg["slot"]
        persistent = msg["pin"] == 1
        pipe.send(msg)
        while True:
            command, args = pipe.recv()
            if command == "exit":
                return
            if command == "disconnect":
                conn.close()
                pipe.send(True)
            elif command == "poll":
                pipe.send(bool(select.select([conn], [], [], args)[0]))
            elif command == "receive":
                pipe.send(receive())
            elif command == "send":
                send(**args)
                pipe.send(True)
            elif command == "exchange":
                send(**args)
                pipe.send(receive())
            elif command == "cycles":
                send(2, workers=1)
                assert receive()["status"] == 0
                for _ in range(args):
                    send(3)
                    grant = receive()
                    assert grant["status"] == 0 and grant["workers"] in (4, 8)
                    send(5)
                    assert receive()["status"] == 0
                send(2, workers=0)
                assert receive()["status"] == 0
                pipe.send(args)
    except Exception as exc:
        pipe.send({"error": repr(exc)})
    finally:
        conn.close()


class ControllerCase(unittest.TestCase):
    lifetime = "graph"
    allocation = "equal"
    decode_wait_ms = 0
    decode_floor_on_prefill = False
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="dt-test-")
        self.path = str(Path(self.temp.name) / "control.sock")
        self.log = open(Path(self.temp.name) / "controller.log", "w+")
        command = [            str(BUILD / "dt_controller_host"),
                                           "--socket", self.path, "--mode", "dynamic",
                                           "--grant-lifetime", self.lifetime,
                                           "--allocation-policy", self.allocation,
                                           "--decode-wait-ms", str(self.decode_wait_ms),
                                           "--events", str(Path(self.temp.name) / "events.csv")]
        if self.decode_floor_on_prefill:
            command.append("--decode-floor-on-prefill")
        self.controller = subprocess.Popen(command,
                                          stdout=self.log, stderr=subprocess.STDOUT)
        self.actors = []
        deadline = time.monotonic() + 5
        while not Path(self.path).exists():
            if self.controller.poll() is not None or time.monotonic() > deadline:
                self.fail("controller did not start")
            time.sleep(.01)

    def tearDown(self):
        for proc, pipe in self.actors:
            if proc.is_alive():
                pipe.send(("exit", None))
            proc.join(3)
            if proc.is_alive():
                proc.kill()
                proc.join()
            pipe.close()
        # Give pidfd notifications an event-loop turn before requesting shutdown.
        time.sleep(.05)
        if self.controller.poll() is None:
            self.controller.send_signal(signal.SIGTERM)
        rc = self.controller.wait(5)
        self.log.seek(0)
        output = self.log.read()
        self.log.close()
        if os.environ.get("DT_TEST_EVIDENCE"):
            shutil.copytree(self.temp.name, Path(os.environ["DT_TEST_EVIDENCE"]) / self.id())
        self.temp.cleanup()
        self.assertEqual(rc, 0, output)

    def new(self):
        parent, child = mp.get_context("spawn").Pipe()
        proc = mp.get_context("spawn").Process(target=actor, args=(self.path, child))
        proc.start()
        child.close()
        self.actors.append((proc, parent))
        self.assertTrue(parent.poll(5))
        self.assertEqual(parent.recv()["status"], 0)
        return proc, parent

    def call(self, actor_, command="exchange", **args):
        actor_[1].send((command, args))
        self.assertTrue(actor_[1].poll(10), "actor timed out")
        result = actor_[1].recv()
        self.assertNotIn("error", result if isinstance(result, dict) else {})
        return result

class Gate(ControllerCase):
    def test_single_10000(self):
        a = self.new()
        a[1].send(("cycles", 10000))
        self.assertTrue(a[1].poll(30))
        self.assertEqual(a[1].recv(), 10000)

    def test_dual_10000_each(self):
        a, b = self.new(), self.new()
        for peer in (a, b):
            peer[1].send(("cycles", 10000))
        for peer in (a, b):
            self.assertTrue(peer[1].poll(45))
            self.assertEqual(peer[1].recv(), 10000)

    def test_join_split_and_expand(self):
        a, b = self.new(), self.new()
        self.call(a, op=2, workers=1)
        full = self.call(a, op=3)
        self.assertEqual((full["workers"], full["mask"]), (8, 255))
        self.call(b, op=2, workers=1)
        self.call(b, "send", op=3)
        b[1].send(("poll", .1))
        self.assertFalse(b[1].recv(), "live eight-core grant was reused")
        self.call(a, op=5)
        gb = self.call(b, "receive")
        ga = self.call(a, op=3)
        self.assertEqual((ga["workers"], gb["workers"]), (4, 4))
        self.assertEqual(ga["mask"] & gb["mask"], 0)
        self.call(a, op=5)
        self.call(a, op=2, workers=0)
        self.call(b, op=5)
        self.assertEqual(self.call(b, op=3)["workers"], 8)
        self.call(b, op=5)

    def test_disconnect_quarantines_until_process_exit(self):
        a, b = self.new(), self.new()
        self.call(a, op=2, workers=1)
        self.assertEqual(self.call(a, op=3)["mask"], 255)
        self.call(a, "disconnect")
        self.call(b, op=2, workers=1)
        self.call(b, "send", op=3)
        b[1].send(("poll", .15))
        self.assertFalse(b[1].recv())
        a[0].kill()
        a[0].join()
        grant = self.call(b, "receive")
        self.assertEqual(grant["workers"], 8)
        self.call(b, op=5)

    def test_bad_epoch_rejected(self):
        a = self.new()
        self.call(a, op=2, workers=1)
        self.assertNotEqual(self.call(a, op=3, epoch_override=99)["status"], 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
