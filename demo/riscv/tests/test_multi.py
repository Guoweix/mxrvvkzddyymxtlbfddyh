"""Real socket peers and OpenMP runtimes exercising multi-process ownership."""

import os
from pathlib import Path
import socket
import subprocess
import time

from test_phase import PhaseCase
from test_control import HERE, BUILD
from control_evidence import validate_cpu_ownership


class MultiGate(PhaseCase):
    decode_floor_on_prefill = True

    def setUp(self):
        super().setUp()
        self.clients = {}
        self.held = {}
        self.pending = set()
        self.revoked = set()

    def send(self, key, op, **kw):
        self.call(self.clients[key], "send", op=op, **kw)
        self.pending.add(key)

    def add(self, key, phase=2):
        peer = self.new()
        self.clients[key] = peer
        self.start(peer, phase)
        return peer

    def settle(self, expected):
        deadline, quiet = time.monotonic() + 8, None
        while time.monotonic() < deadline:
            traffic = False
            for key, peer in list(self.clients.items()):
                peer[1].send(("poll", 0))
                if not peer[1].recv():
                    continue
                traffic = True
                msg = self.call(peer, "receive")
                self.assertEqual(msg["status"], 0, msg)
                if msg["op"] == 7:
                    self.held[key] = msg
                elif msg["op"] == 8:
                    # Full-return test actors; the actual runtime test below
                    # also exercises SHRINK and real membership/affinity.
                    self.revoked.add(key)
                elif msg["op"] == 5:
                    self.held.pop(key, None)
                    self.pending.remove(key)
                else:
                    self.assertIn(msg["op"], (2, 9), msg)
                    self.pending.remove(key)
                if key in self.revoked and key not in self.pending:
                    self.send(key, 5)
                    self.revoked.remove(key)
            quiet = None if traffic else quiet or time.monotonic()
            if (
                quiet
                and time.monotonic() - quiet > 0.06
                and set(self.held) == set(expected)
            ):
                masks = [g["mask"] for g in self.held.values()]
                self.assertEqual(sum(masks), sum(set(masks)))
                union = 0
                for mask in masks:
                    self.assertFalse(union & mask)
                    union |= mask
                return
            time.sleep(0.002)
        self.fail(f"allocation did not settle: expected={expected}, held={self.held}")

    def idle_all(self):
        for key in self.clients:
            self.send(key, 2, workers=0)
        self.settle([])
        return validate_cpu_ownership(self.rows())

    def test_incremental_join_idle_quarantine_and_slot_reuse(self):
        for n in range(1, 9):
            self.add(n - 1)
            self.settle(range(n))
            widths = sorted(g["workers"] for g in self.held.values())
            self.assertEqual(sum(widths), 8)
            self.assertLessEqual(max(widths) - min(widths), 1)
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as ninth:
            ninth.settimeout(2)
            ninth.connect(self.path)
            self.assertEqual(ninth.recv(112), b"")
        dead = self.clients.pop(3)
        mask = self.held.pop(3)["mask"]
        slot = 3
        self.call(dead, "disconnect")
        self.settle(self.clients)
        self.assertFalse(any(g["mask"] & mask for g in self.held.values()))
        dead[0].kill()
        dead[0].join()
        self.settle(self.clients)
        self.assertEqual(sum(g["workers"] for g in self.held.values()), 8)
        self.add(8)
        self.settle(self.clients)
        self.assertEqual(self.held[8]["slot"], slot)
        self.assertTrue(self.idle_all()["overlap_free"])

    def test_prefill_floors_and_coalesced_phase_changes(self):
        for i in range(4):
            self.add(i)
        self.settle(range(4))
        self.send(0, 9, phase=1, prompt=128)
        self.settle(range(4))
        self.assertEqual([self.held[i]["workers"] for i in range(4)], [5, 1, 1, 1])
        self.send(1, 9, phase=1, prompt=128)
        self.settle(range(4))
        self.assertEqual([self.held[i]["workers"] for i in range(4)], [3, 3, 1, 1])
        for i in range(4, 8):
            self.add(i)
        self.settle(range(8))
        self.assertEqual([self.held[i]["workers"] for i in range(8)], [1] * 8)
        for i in (0, 1):
            self.send(i, 9, phase=2, decode=1)
        self.settle(range(8))
        self.assertTrue(self.idle_all()["overlap_free"])

    def test_eight_actual_openmp_clients(self):
        if not set(range(8)).issubset(os.sched_getaffinity(0)):
            self.skipTest("actual CPU0-7 required")
        processes, logs = [], []
        try:
            for i in range(8):
                log = (Path(self.temp.name) / f"runtime-{i}.log").open("w")
                logs.append(log)
                processes.append(
                    subprocess.Popen(
                        [
                            str(BUILD / "dt_phase_probe_host"),
                            self.path,
                            "300",
                            str(50 if i % 2 else 0),
                            "1000",
                        ],
                        stdout=log,
                        stderr=log,
                    )
                )
            for p in processes:
                self.assertEqual(p.wait(40), 0)
            rows = self.rows()
            self.assertEqual(
                {r["slot"] for r in rows if r["event"] == "HELLO"},
                set(map(str, range(8))),
            )
            self.assertTrue(any(r["event"] == "SHRINK" for r in rows))
            self.assertTrue(validate_cpu_ownership(rows)["overlap_free"])
        finally:
            for p in processes:
                if p.poll() is None:
                    p.kill()
                p.wait()
            for log in logs:
                log.close()


class GraphCapacity(PhaseCase):
    lifetime = "graph"
    allocation = "equal"

    def test_third_graph_client_rejected(self):
        self.new()
        self.new()
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as third:
            third.settimeout(2)
            third.connect(self.path)
            self.assertEqual(third.recv(112), b"")
