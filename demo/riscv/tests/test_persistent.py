#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Persistent ownership, event delivery and actual OpenMP lifecycle gates."""

import csv
import os
from pathlib import Path
import subprocess
import time
import unittest

from test_control import ControllerCase, HERE, BUILD


class PersistentGate(ControllerCase):
    lifetime = "persistent"

    def receive(self, peer, op):
        result = self.call(peer, "receive")
        self.assertEqual(result["op"], op, result)
        self.assertEqual(result["status"], 0, result)
        return result

    def exchange(self, peer, op, **kwargs):
        result = self.call(peer, op=op, **kwargs)
        self.assertEqual(result["op"], op, result)
        self.assertEqual(result["status"], 0, result)
        return result

    def start(self, peer):
        self.exchange(peer, 2, workers=1)
        self.exchange(peer, 3)

    def no_message(self, peer, duration=0.05):
        peer[1].send(("poll", duration))
        self.assertFalse(peer[1].recv(), "unexpected notification / overlapping grant")

    def rows(self):
        with (Path(self.temp.name) / "events.csv").open() as stream:
            return list(csv.DictReader(stream))

    def probe(self, count, kind="stable", compute_us=0):
        if not set(range(8)).issubset(os.sched_getaffinity(0)):
            self.skipTest("actual CPU0-7 required")
        result = subprocess.run(
            [
                str(BUILD / "dt_persistent_probe_host"),
                self.path,
                str(count),
                str(compute_us),
                kind,
            ],
            capture_output=True,
            text=True,
            timeout=90,
        )
        (Path(self.temp.name) / "probe.log").write_text(result.stdout + result.stderr)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout[-2000:])
        self.assertIn(f"PASS iterations={count}", result.stdout)
        return self.rows()

    def test_stable_10000_graphs_one_registration(self):
        rows = self.probe(10000)
        for event in ("REQUEST", "GRANT", "MEMBERS", "UNMEMBERS", "RELEASE"):
            self.assertEqual(sum(row["event"] == event for row in rows), 1, event)

    def test_actual_width_and_recreated_members(self):
        rows = self.probe(1000, "vary")
        self.assertEqual(sum(row["event"] == "GRANT" for row in rows), 1)
        self.assertGreater(sum(row["event"] == "MEMBERS" for row in rows), 500)

    def test_actual_worker_exit_and_tid_lifetime(self):
        rows = self.probe(100, "recreate")
        self.assertEqual(sum(row["event"] == "GRANT" for row in rows), 1)
        self.assertEqual(sum(row["event"] == "MEMBERS" for row in rows), 100)

    def test_join_split_expand_and_quiescent_handoff(self):
        a, b = self.new(), self.new()
        self.start(a)
        self.assertEqual(self.receive(a, 7)["mask"], 255)
        self.exchange(a, 4, workers=1)
        self.start(b)
        self.receive(a, 8)
        self.no_message(b)
        self.exchange(a, 6)
        self.no_message(b)  # UNMEMBERS is not RELEASE.
        self.exchange(a, 5)
        ga, gb = self.receive(a, 7), self.receive(b, 7)
        self.assertEqual((ga["workers"], gb["workers"]), (4, 4))
        self.assertEqual(ga["mask"] & gb["mask"], 0)
        self.exchange(a, 2, workers=0)
        self.receive(a, 8)
        self.receive(b, 8)
        self.exchange(b, 5)
        self.no_message(b)  # A still owns its four CPUs.
        self.exchange(a, 5)
        self.assertEqual(self.receive(b, 7)["mask"], 255)

    def test_cancel_pending_and_coalesce(self):
        a, b = self.new(), self.new()
        self.start(a)
        self.receive(a, 7)
        self.start(b)
        self.receive(a, 8)
        self.exchange(b, 2, workers=0)
        self.exchange(a, 5)
        self.assertEqual(self.receive(a, 7)["workers"], 8)
        self.no_message(b)

    def test_cancel_already_delivered_grant(self):
        a = self.new()
        self.start(a)
        # GRANT queued before IDLE: it must still be returned safely.
        self.call(a, "send", op=2, workers=0)
        self.receive(a, 7)
        self.receive(a, 2)
        self.receive(a, 8)
        self.exchange(a, 5)
        self.no_message(a)

    def test_disconnect_quarantine(self):
        a, b = self.new(), self.new()
        self.start(a)
        self.receive(a, 7)
        self.call(a, "disconnect")
        self.start(b)
        self.no_message(b)
        a[0].kill()
        a[0].join()
        self.assertEqual(self.receive(b, 7)["workers"], 8)

    def test_old_generation_is_rejected(self):
        a = self.new()
        self.start(a)
        self.receive(a, 7)
        result = self.call(a, op=5, epoch_override=0)
        self.assertNotEqual(result["status"], 0)

    def test_resource_wait_has_no_30_second_timeout(self):
        a, b = self.new(), self.new()
        self.start(a)
        self.receive(a, 7)
        self.start(b)
        self.receive(a, 8)
        self.no_message(b, 31.2)
        self.assertIsNone(self.controller.poll())
        self.exchange(a, 5)
        self.assertEqual(self.receive(b, 7)["workers"], 4)
        self.receive(a, 7)


if __name__ == "__main__":
    unittest.main(verbosity=2, failfast=True)
