#!/usr/bin/env python3

import unittest
from types import SimpleNamespace

from workload import (
    Workload,
    build_equal_prompt_tokens,
    select_prompts,
    validate_prompt_manifest,
)


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


class PromptManifestTest(unittest.TestCase):
    def test_short_smoke_manifest_is_valid(self):
        validate_prompt_manifest([{"id": "fixed-1", "prompt": "one", "target_tokens": 4}])

    def test_duplicate_prompt_ids_are_rejected(self):
        with self.assertRaises(ValueError):
            validate_prompt_manifest(
                [
                    {"id": "same", "prompt": "one", "target_tokens": 4},
                    {"id": "same", "prompt": "two", "target_tokens": 4},
                ]
            )

    def test_mixed_target_lengths_are_rejected(self):
        with self.assertRaises(ValueError):
            validate_prompt_manifest(
                [
                    {"id": "one", "prompt": "one", "target_tokens": 4},
                    {"id": "two", "prompt": "two", "target_tokens": 5},
                ]
            )

    def test_request_count_selects_fixed_prefix(self):
        prompts = [{"id": "short"}, {"id": "medium"}, {"id": "long"}]
        self.assertEqual(
            [item["id"] for item in select_prompts(prompts, 2)],
            ["short", "medium"],
        )

    def test_request_count_rejects_zero(self):
        with self.assertRaises(ValueError):
            select_prompts([{"id": "short"}], 0)

    def test_distinct_prompts_are_padded_to_one_exact_length(self):
        prompts = [
            {"id": "one", "prompt": "first", "target_tokens": 5},
            {"id": "two", "prompt": "second", "target_tokens": 5},
        ]
        source = {
            "A": {"one": [1, 2, 3], "two": [1, 8, 9, 10]},
            "B": {"one": [4, 5], "two": [4, 7, 8]},
        }

        result = build_equal_prompt_tokens(prompts, source)

        self.assertEqual(result["A"]["one"], [1, 2, 3, 3, 3])
        self.assertEqual(result["A"]["two"], [1, 8, 9, 10, 10])
        self.assertEqual(result["B"]["one"], [4, 5, 5, 5, 5])
        self.assertTrue(
            all(len(tokens) == 5 for model in result.values() for tokens in model.values())
        )

    def test_prompt_longer_than_target_is_rejected(self):
        prompts = [{"id": "one", "prompt": "first", "target_tokens": 2}]
        with self.assertRaises(ValueError):
            build_equal_prompt_tokens(prompts, {"A": {"one": [1, 2, 3]}})


if __name__ == "__main__":
    unittest.main()
