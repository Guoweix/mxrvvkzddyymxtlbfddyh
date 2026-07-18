#!/usr/bin/env python3

import unittest
from types import SimpleNamespace

from workload import Workload


class EventSink:
    def __init__(self):
        self.events = []

    def write(self, event):
        self.events.append(event)


def workload_for_hook(command):
    workload = Workload.__new__(Workload)
    workload.args = SimpleNamespace(
        injection_hook=command,
        injection_hook_timeout=2.0,
        incumbent_model="A",
        injected_model="B",
    )
    workload.events = EventSink()
    return workload


class InjectionHookTest(unittest.TestCase):
    def test_hook_receives_model_and_team_environment(self):
        workload = workload_for_hook('printf "%s:%s" "$INJECTED_MODEL" "$INJECTED_TEAM"')

        result = workload.run_injection_hook()

        self.assertTrue(result["ok"])
        self.assertEqual(result["stdout"], "B:1")
        self.assertEqual(workload.events.events[0]["type"], "injection_hook")

    def test_nonzero_hook_is_invalid(self):
        workload = workload_for_hook("exit 7")

        result = workload.run_injection_hook()

        self.assertFalse(result["ok"])
        self.assertEqual(result["return_code"], 7)


if __name__ == "__main__":
    unittest.main()
