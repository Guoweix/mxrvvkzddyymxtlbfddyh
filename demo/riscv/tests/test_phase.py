#!/usr/bin/env python3
"""Strict prefill priority, safe phase handoffs and real runtime boundary tests."""

import csv
import os
from pathlib import Path
import subprocess
import time
import unittest

from test_control import ControllerCase, HERE, BUILD


class PhaseCase(ControllerCase):
    lifetime = "persistent"
    allocation = "prefill-first"

    def receive(self, peer, op):
        result = self.call(peer, "receive")
        self.assertEqual((result["op"], result["status"]), (op, 0), result)
        return result

    def exchange(self, peer, op, **kw):
        result = self.call(peer, op=op, **kw)
        self.assertEqual((result["op"], result["status"]), (op, 0), result)
        return result

    def start(self, peer, phase, prompt=None, decode=None):
        self.exchange(peer, 2, workers=1)
        self.exchange(
            peer,
            3,
            phase=phase,
            prompt=(128 if phase in (1, 3) else 0) if prompt is None else prompt,
            decode=(1 if phase in (2, 3) else 0) if decode is None else decode,
        )

    def phase(self, peer, phase, revoke=False):
        self.call(
            peer,
            "send",
            op=9,
            phase=phase,
            prompt=128 if phase in (1, 3) else 0,
            decode=1 if phase in (2, 3) else 0,
        )
        if revoke:
            self.receive(
                peer, 8
            )  # Must precede ACK, not arrive after computation resumes.
        return self.receive(peer, 9)

    def no_message(self, peer, duration=0.05):
        peer[1].send(("poll", duration))
        self.assertFalse(peer[1].recv(), "unexpected/overlapping grant")

    def rows(self):
        with (Path(self.temp.name) / "events.csv").open() as stream:
            return list(csv.DictReader(stream))


class PhaseGate(PhaseCase):
    def test_prefill_takes_all_and_decode_resumes(self):
        a, b = self.new(), self.new()
        self.start(a, 2, decode=4)  # Multi-token decode is still decode.
        self.assertEqual(self.receive(a, 7)["workers"], 8)
        self.exchange(a, 4, workers=1)
        self.start(b, 1, prompt=1)  # One uncached prompt token is still prefill.
        self.receive(a, 8)
        self.no_message(b)
        self.exchange(a, 6)
        self.no_message(b)
        self.exchange(a, 5)
        self.assertEqual(self.receive(b, 7)["mask"], 255)
        self.no_message(a)  # Earlier pending decode cannot block the prefill waiter.
        self.phase(b, 2, revoke=True)
        self.exchange(b, 5)
        ga, gb = self.receive(a, 7), self.receive(b, 7)
        self.assertEqual((ga["workers"], gb["workers"]), (4, 4))
        self.assertEqual(ga["mask"] & gb["mask"], 0)

    def test_dual_prefill_phase_before_wait_and_coalescing(self):
        a, b = self.new(), self.new()
        self.start(a, 1)
        self.receive(a, 7)
        self.start(b, 1)
        self.receive(a, 8)
        self.exchange(a, 5)
        self.assertEqual(self.receive(a, 7)["workers"], 4)
        self.assertEqual(self.receive(b, 7)["workers"], 4)
        self.phase(a, 2, revoke=True)
        self.receive(b, 8)
        self.exchange(a, 5)
        self.exchange(b, 5)
        self.assertEqual(self.receive(b, 7)["workers"], 8)
        self.no_message(a)
        self.phase(b, 2, revoke=True)
        self.exchange(b, 5)
        self.assertEqual(self.receive(a, 7)["workers"], 4)
        self.assertEqual(self.receive(b, 7)["workers"], 4)

    def test_mixed_priority_and_idle_release(self):
        a, b = self.new(), self.new()
        self.start(a, 3)
        self.receive(a, 7)
        self.start(b, 2)
        self.no_message(b)
        self.no_message(a)  # No unnecessary revoke when target remains eight.
        self.exchange(a, 2, workers=0)
        self.receive(a, 8)
        self.no_message(b)
        self.exchange(a, 5)
        self.assertEqual(self.receive(b, 7)["workers"], 8)

    def test_pending_phase_changes_cancel_stale_priority(self):
        a, b = self.new(), self.new()
        self.start(a, 2)
        self.receive(a, 7)
        self.start(b, 1)
        self.receive(a, 8)
        self.phase(b, 2)  # Change before its first grant: no stale eight-core grant.
        self.exchange(a, 5)
        self.assertEqual(self.receive(a, 7)["workers"], 4)
        self.assertEqual(self.receive(b, 7)["workers"], 4)

    def test_unknown_phase_rejected(self):
        a = self.new()
        self.exchange(a, 2, workers=1)
        self.assertNotEqual(self.call(a, op=3)["status"], 0)

    def test_invalid_mixed_rejected(self):
        a = self.new()
        self.exchange(a, 2, workers=1)
        self.assertNotEqual(self.call(a, op=3, phase=3, prompt=1)["status"], 0)

    def test_long_prefill_no_ttl_and_decode_resumes(self):
        a, b = self.new(), self.new()
        self.start(a, 1)
        self.receive(a, 7)
        self.start(b, 2)
        self.no_message(b, 31.2)
        self.assertIsNone(self.controller.poll())
        self.phase(a, 2, revoke=True)
        self.exchange(a, 5)
        self.assertEqual(self.receive(a, 7)["workers"], 4)
        self.assertEqual(self.receive(b, 7)["workers"], 4)

    def test_actual_runtime_10000_graphs_no_token_rpc(self):
        if not set(range(8)).issubset(os.sched_getaffinity(0)):
            self.skipTest("actual CPU0-7 required")
        result = subprocess.run(
            [str(BUILD / "dt_phase_probe_host"), self.path, "10000", "100", "0"],
            capture_output=True,
            text=True,
            timeout=60,
        )
        (Path(self.temp.name) / "probe.log").write_text(result.stdout + result.stderr)
        self.assertEqual(result.returncode, 0, result.stdout[-2000:] + result.stderr)
        rows = self.rows()
        for event in ("GRANT", "MEMBERS", "UNMEMBERS", "RELEASE"):
            self.assertEqual(sum(row["event"] == event for row in rows), 1, event)
        self.assertEqual(sum(row["event"] == "PHASE" for row in rows), 2)

    def test_actual_runtime_dual_handoff(self):
        if not set(range(8)).issubset(os.sched_getaffinity(0)):
            self.skipTest("actual CPU0-7 required")
        logs, processes = [], []
        try:
            for index, prefill in enumerate((0, 10)):
                log = (Path(self.temp.name) / f"probe-{index}.log").open("w+")
                logs.append(log)
                processes.append(
                    subprocess.Popen(
                        [
                            str(BUILD / "dt_phase_probe_host"),
                            self.path,
                            "60",
                            str(prefill),
                            "10000",
                        ],
                        stdout=log,
                        stderr=log,
                    )
                )
                if index == 0:
                    deadline = time.monotonic() + 5
                    while not any(row["event"] == "MEMBERS" for row in self.rows()):
                        self.assertLess(time.monotonic(), deadline)
                        time.sleep(0.01)
            for proc in processes:
                self.assertEqual(proc.wait(30), 0)
            rows = self.rows()
            self.assertTrue(
                any(
                    r["event"] == "TARGET"
                    and r["phase"] == "2"
                    and r["target_workers"] == "0"
                    for r in rows
                )
            )
            self.assertTrue(
                any(
                    r["event"] == "GRANT" and r["phase"] == "1" and r["workers"] == "8"
                    for r in rows
                )
            )
            self.assertTrue(
                any(
                    r["event"] in ("GRANT", "SHRINK")
                    and r["phase"] == "2"
                    and r["workers"] == "4"
                    for r in rows
                )
            )
        finally:
            for proc in processes:
                if proc.poll() is None:
                    proc.kill()
                proc.wait()
            for log in logs:
                log.close()


if __name__ == "__main__":
    unittest.main(verbosity=2, failfast=True)
