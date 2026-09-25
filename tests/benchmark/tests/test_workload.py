import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from bench.workload import (  # noqa: E402
    build_equal_prompt_tokens,
    select_prompts,
    validate_prompt_manifest,
)


PROMPTS = [
    {"id": "a", "target_tokens": 4, "prompt": "alpha beta"},
    {"id": "b", "target_tokens": 4, "prompt": "gamma delta epsilon zeta eta"},
]


class BuildEqualPromptTokensTest(unittest.TestCase):
    def test_pads_to_target_length(self):
        source = {"A": {"a": [1, 2], "b": [3, 4]}, "B": {"a": [5], "b": [6, 7, 8]}}
        result = build_equal_prompt_tokens(PROMPTS, source)
        self.assertEqual(result["A"]["a"], [1, 2, 2, 2])
        self.assertEqual(result["A"]["b"], [3, 4, 4, 4])
        self.assertEqual(result["B"]["a"], [5, 5, 5, 5])
        self.assertEqual(result["B"]["b"], [6, 7, 8, 8])

    def test_rejects_overflow(self):
        source = {"A": {"a": [1, 2, 3, 4, 5], "b": [1]}}
        with self.assertRaises(ValueError):
            build_equal_prompt_tokens(PROMPTS, source)

    def test_rejects_empty_tokenization(self):
        source = {"A": {"a": [], "b": [1]}}
        with self.assertRaises(ValueError):
            build_equal_prompt_tokens(PROMPTS, source)


class SelectPromptsTest(unittest.TestCase):
    def test_selects_prefix(self):
        self.assertEqual(select_prompts(PROMPTS, 1), PROMPTS[:1])
        self.assertEqual(select_prompts(PROMPTS, 2), PROMPTS)

    def test_rejects_out_of_range(self):
        with self.assertRaises(ValueError):
            select_prompts(PROMPTS, 0)
        with self.assertRaises(ValueError):
            select_prompts(PROMPTS, 3)


class ValidateManifestTest(unittest.TestCase):
    def test_accepts_valid(self):
        validate_prompt_manifest(PROMPTS)

    def test_rejects_mismatched_targets(self):
        bad = [dict(PROMPTS[0]), dict(PROMPTS[1], target_tokens=8)]
        with self.assertRaises(ValueError):
            validate_prompt_manifest(bad)


if __name__ == "__main__":
    unittest.main()
