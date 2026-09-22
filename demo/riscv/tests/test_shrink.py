#!/usr/bin/env python3
"""Partial returns retain ownership; stale targets never authorize overlap."""
import unittest

from test_phase import PhaseCase
from control_evidence import validate_cpu_ownership


class ShrinkGate(PhaseCase):
    allocation = "equal"

    def setup_split(self, members=True):
        a, b = self.new(), self.new()
        self.start(a, 2)
        initial = self.receive(a, 7)
        if members:
            self.exchange(a, 4, workers=1)
        self.start(b, 2)
        target = self.receive(a, 8)
        self.assertEqual((target["workers"], target["mask"]), (4, 15))
        self.assertEqual(target["epoch"], initial["epoch"])
        self.no_message(b)
        return a, b, initial

    def test_partial_return_and_retained_quarantine(self):
        a, b, initial = self.setup_split()
        self.exchange(a, 6)
        self.no_message(b)  # Neither REVOKE nor UNMEMBERS returns the old CPUs.
        ack = self.exchange(a, 10, workers=4, mask=15)
        self.assertEqual(ack["epoch"], initial["epoch"])
        self.assertEqual(self.receive(b, 7)["mask"], 240)
        self.no_message(a)  # The retained four CPUs need no new GRANT.
        self.exchange(a, 4, workers=1)
        self.call(a, "disconnect")
        self.no_message(b)  # CPUs 0..3 remain quarantined while A lives.
        a[0].kill()
        a[0].join()
        self.assertEqual(self.receive(b, 8)["mask"], 255)
        self.exchange(b, 5)
        self.assertEqual(self.receive(b, 7)["mask"], 255)
        self.exchange(b, 2, workers=0)
        self.receive(b, 8)
        self.exchange(b, 5)
        self.assertEqual(validate_cpu_ownership(self.rows())["shrinks_checked"], 1)

    def test_changed_target_after_revoke_is_reconciled(self):
        a, b, _ = self.setup_split(members=False)
        self.exchange(b, 2, workers=0)
        # A has already prepared the old shrink. Safely accept the subset,
        # then send the latest target after ACK, without overlapping grants.
        self.exchange(a, 10, workers=4, mask=15)
        self.assertEqual(self.receive(a, 8)["mask"], 255)
        self.exchange(a, 5)
        self.assertEqual(self.receive(a, 7)["workers"], 8)
        self.no_message(b)

    def test_cannot_shrink_published_members(self):
        a, b, _ = self.setup_split()
        self.assertNotEqual(self.call(a, op=10, workers=4, mask=15)["status"], 0)
        self.no_message(b)

    def test_cannot_shrink_to_unowned_cpu(self):
        a, b, _ = self.setup_split(members=False)
        self.exchange(a, 10, workers=4, mask=15)
        self.receive(b, 7)
        self.exchange(b, 2, workers=0)
        self.receive(b, 8)
        self.receive(a, 8)  # Latest target expands A; it still only owns 0..3.
        self.assertNotEqual(self.call(a, op=10, workers=1, mask=128)["status"], 0)


if __name__ == "__main__":
    unittest.main(verbosity=2, failfast=True)
