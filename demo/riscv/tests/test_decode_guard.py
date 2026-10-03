#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Decode wait -> sticky floor, graph-boundary ownership, and readiness timing."""
import time
import unittest

from test_phase import PhaseCase


class DecodeGuard(PhaseCase):
    decode_wait_ms = 200

    def wait_event(self, kind, count=1):
        deadline = time.monotonic() + 3
        while True:
            rows = [r for r in self.rows() if r["event"] == kind]
            if len(rows) >= count:
                return rows[-1]
            self.assertLess(time.monotonic(), deadline, f"missing {kind}")
            time.sleep(.01)

    def protected_pair(self):
        a, b = self.new(), self.new()
        self.start(a, 2)
        self.receive(a, 7)
        self.exchange(a, 4, workers=1)
        self.start(b, 1)
        self.receive(a, 8)
        self.exchange(a, 6)
        self.exchange(a, 5)
        self.receive(b, 7)
        self.exchange(b, 4, workers=1)
        self.receive(b, 8)  # timer promotes decode and revokes prefill's full grant
        self.no_message(a)
        self.exchange(b, 6)
        self.no_message(a)  # UNMEMBERS is not a RELEASE
        self.exchange(b, 5)
        ga, gb = self.receive(a, 7), self.receive(b, 7)
        self.assertEqual((ga["workers"], gb["workers"]), (1, 7))
        self.assertEqual(ga["mask"] & gb["mask"], 0)
        self.assertEqual(ga["mask"] | gb["mask"], 255)
        self.exchange(a, 4, workers=1)
        return a, b

    def test_floor_sticky_across_phases_and_reset_only_at_idle(self):
        a, b = self.protected_pair()
        for phase in (3, 2):  # MIXED uses prefill priority but does not clear the latch
            self.phase(a, phase, revoke=True)
            self.receive(b, 8)
            self.exchange(a, 6)
            self.exchange(a, 5)
            self.exchange(b, 5)
            ga, gb = self.receive(a, 7), self.receive(b, 7)
            self.assertEqual((ga["workers"], gb["workers"]),
                             (4, 4) if phase == 3 else (1, 7))
            self.exchange(a, 4, workers=1)
        before = sum(r["event"] in ("WIRE_RX", "WIRE_TX") for r in self.rows())
        self.no_message(a, .3)
        self.no_message(b, .1)
        self.assertEqual(before, sum(r["event"] in ("WIRE_RX", "WIRE_TX") for r in self.rows()))
        self.assertEqual(sum(r["event"] == "DECODE_PROTECT" for r in self.rows()), 1)
        self.exchange(a, 2, workers=0)
        self.receive(a, 8)
        self.receive(b, 8)
        self.exchange(a, 6)
        self.exchange(a, 5)
        self.exchange(b, 5)
        self.assertEqual(self.receive(b, 7)["workers"], 8)
        self.start(a, 2)  # a new active episode has no inherited floor
        self.no_message(a, .05)
        self.assertEqual(self.rows()[-1]["decode_protected"], "0")
        self.wait_event("DECODE_PROTECT", 2)
        self.receive(b, 8)

    def test_wait_excludes_running_graph_and_ends_at_ready_not_grant(self):
        a, b = self.new(), self.new()
        self.start(a, 2)
        self.receive(a, 7)
        self.start(b, 1)
        self.receive(a, 8)
        # REVOKE may race with a MEMBERS RPC; this graph can still compute.
        self.exchange(a, 4, workers=1)
        self.no_message(b, .35)  # a is still computing with its old grant
        self.assertFalse(any(r["event"] == "DECODE_PROTECT" for r in self.rows()))
        self.exchange(a, 6)
        self.exchange(a, 5)
        self.receive(b, 7)
        for _ in range(3):
            self.phase(a, 2)  # repeated intent must not reset the same wait
            time.sleep(.04)
        self.receive(b, 8)
        promoted = self.wait_event("DECODE_PROTECT")
        self.assertGreaterEqual(int(promoted["decode_wait_ns"]), 200_000_000)
        self.no_message(a, .3)  # long prefill: soft budget, never unsafe CPU stealing
        self.assertIsNone(self.controller.poll())
        self.exchange(b, 5)
        self.assertEqual(self.receive(a, 7)["workers"], 1)
        self.receive(b, 7)
        self.no_message(a, .1)  # GRANT received, workers still not registered
        self.assertEqual(sum(r["event"] == "DECODE_RESUME" for r in self.rows()), 1)
        self.exchange(a, 4, workers=1)
        resumed = self.wait_event("DECODE_RESUME", 2)
        self.assertGreater(int(resumed["decode_over_budget_ns"]), 300_000_000)
        self.assertGreater(int(resumed["decode_recovery_ns"]), 300_000_000)
        # When prefill ends, protection no longer biases decode's equal shares.
        self.phase(b, 2, revoke=True)
        self.receive(a, 8)
        self.exchange(a, 6)
        self.exchange(a, 5)
        self.exchange(b, 5)
        self.assertEqual(self.receive(a, 7)["workers"], 4)
        self.assertEqual(self.receive(b, 7)["workers"], 4)


class ImmediateDecodeFloor(PhaseCase):
    decode_wait_ms = 0
    decode_floor_on_prefill = True

    def test_prefill_is_first_granted_seven_not_eight(self):
        decode, prefill = self.new(), self.new()
        self.start(decode, 2)
        self.assertEqual(self.receive(decode, 7)["workers"], 8)
        self.exchange(decode, 4, workers=1)
        self.start(prefill, 1)
        self.receive(decode, 8)
        self.no_message(prefill)
        self.exchange(decode, 6)
        self.exchange(decode, 5)
        gp, gd = self.receive(prefill, 7), self.receive(decode, 7)
        self.assertEqual((gp["workers"], gd["workers"]), (7, 1))
        self.assertEqual(gp["mask"] & gd["mask"], 0)
        self.assertEqual(gp["mask"] | gd["mask"], 255)
        rows = self.rows()
        prefill_grants = [r for r in rows if r["event"] == "GRANT" and r["phase"] == "1"]
        self.assertTrue(prefill_grants)
        self.assertFalse(any(r["workers"] == "8" for r in prefill_grants))
        self.assertFalse(any(r["event"] == "DECODE_PROTECT" for r in rows))


if __name__ == "__main__":
    unittest.main(verbosity=2, failfast=True)
