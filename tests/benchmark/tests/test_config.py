import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from bench.runner import validate_models  # noqa: E402


def model(model_id, port, **overrides):
    base = {
        "id": model_id,
        "port": port,
        "model": "/mnt/llm/llama/models/example.gguf",
        "threads": 4,
        "threads_batch": 4,
        "requests": 1,
    }
    base.update(overrides)
    return base


class ValidateModelsTest(unittest.TestCase):
    def test_accepts_valid(self):
        config = {"models": [model("A", 9191), model("B", 9192, cpu_mask=15)]}
        self.assertEqual(len(validate_models(config)), 2)

    def test_rejects_duplicate_port(self):
        config = {"models": [model("A", 9191), model("B", 9191)]}
        with self.assertRaises(ValueError):
            validate_models(config)

    def test_rejects_bad_port(self):
        with self.assertRaises(ValueError):
            validate_models({"models": [model("A", 80)]})

    def test_rejects_relative_model_path(self):
        with self.assertRaises(ValueError):
            validate_models({"models": [model("A", 9191, model="example.gguf")]})

    def test_rejects_too_many_threads(self):
        with self.assertRaises(ValueError):
            validate_models({"models": [model("A", 9191, threads=9)]})

    def test_rejects_empty(self):
        with self.assertRaises(ValueError):
            validate_models({"models": []})


if __name__ == "__main__":
    unittest.main()
