#!/usr/bin/env python3
"""Deterministic asynchronous and symmetric dual-server streaming workloads."""

import argparse
import hashlib
import json
import os
import subprocess
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path


def post_json(url, payload, timeout):
    request = urllib.request.Request(
        url,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    return urllib.request.urlopen(request, timeout=timeout)


def tokenize(base_url, prompt, timeout):
    with post_json(f"{base_url}/tokenize", {"content": prompt}, timeout) as response:
        payload = json.load(response)
    tokens = payload.get("tokens")
    if not isinstance(tokens, list):
        raise RuntimeError(f"invalid /tokenize response from {base_url}")
    return tokens


def wait_until(target_ns):
    while True:
        remaining = (target_ns - time.monotonic_ns()) / 1_000_000_000
        if remaining <= 0:
            return
        time.sleep(min(remaining, 0.05))


class EventWriter:
    def __init__(self, path):
        self.output = open(path, "w", buffering=1)
        self.lock = threading.Lock()

    def write(self, event):
        with self.lock:
            self.output.write(json.dumps(event, separators=(",", ":")) + "\n")

    def close(self):
        self.output.close()


class Workload:
    def __init__(self, args, prompts, prompt_tokens):
        self.args = args
        self.prompts = prompts
        self.prompt_tokens = prompt_tokens
        self.events = EventWriter(args.events)
        self.condition = threading.Condition()
        self.token_counts = {}
        self.results = {}

    def run_request(self, role, model, base_url, prompt, sequence, release_ns):
        request_id = f"{role}-{prompt['id']}"
        wait_until(release_ns)
        send_ns = time.monotonic_ns()
        send_wall_ns = time.time_ns()
        token_ids = []
        token_times_ns = []
        token_wall_times_ns = []
        timings = {}
        completed = False
        error = ""

        self.events.write(
            {
                "type": "request_start",
                "request_id": request_id,
                "role": role,
                "model": model,
                "prompt_id": prompt["id"],
                "sequence": sequence,
                "scheduled_release_ns": release_ns,
                "monotonic_ns": send_ns,
                "wall_time_ns": send_wall_ns,
            }
        )

        payload = {
            "prompt": self.prompt_tokens[model][prompt["id"]],
            "n_predict": self.args.output_tokens,
            "temperature": 0,
            "top_k": 1,
            "top_p": 1.0,
            "seed": self.args.seed,
            "ignore_eos": True,
            "cache_prompt": False,
            "stream": True,
        }

        try:
            with post_json(f"{base_url}/completion", payload, self.args.request_timeout) as response:
                for raw_line in response:
                    line = raw_line.decode("utf-8", errors="replace").strip()
                    if not line.startswith("data:"):
                        continue
                    data = line[5:].strip()
                    if not data or data == "[DONE]":
                        continue
                    try:
                        chunk = json.loads(data)
                    except json.JSONDecodeError:
                        continue

                    chunk_tokens = chunk.get("tokens") or []
                    if isinstance(chunk_tokens, list):
                        chunk_ns = time.monotonic_ns()
                        chunk_wall_ns = time.time_ns()
                        for token_id in chunk_tokens:
                            token_ns = chunk_ns
                            token_index = len(token_ids) + 1
                            token_ids.append(int(token_id))
                            token_times_ns.append(token_ns)
                            token_wall_times_ns.append(chunk_wall_ns)
                            self.events.write(
                                {
                                    "type": "token",
                                    "request_id": request_id,
                                    "role": role,
                                    "model": model,
                                    "prompt_id": prompt["id"],
                                    "token_index": token_index,
                                    "token_id": int(token_id),
                                    "monotonic_ns": token_ns,
                                    "wall_time_ns": chunk_wall_ns,
                                }
                            )
                            with self.condition:
                                self.token_counts[request_id] = token_index
                                self.condition.notify_all()

                    if chunk.get("stop"):
                        completed = True
                        timings = chunk.get("timings") or {}
        except (OSError, TimeoutError, urllib.error.URLError) as exc:
            error = f"{type(exc).__name__}: {exc}"
        except Exception as exc:  # Preserve a complete invalid-run record.
            error = f"{type(exc).__name__}: {exc}"

        finish_ns = time.monotonic_ns()
        result = {
            "request_id": request_id,
            "role": role,
            "model": model,
            "base_url": base_url,
            "prompt_id": prompt["id"],
            "sequence": sequence,
            "scheduled_release_ns": release_ns,
            "input_tokens": len(self.prompt_tokens[model][prompt["id"]]),
            "requested_output_tokens": self.args.output_tokens,
            "output_tokens": len(token_ids),
            "token_ids": token_ids,
            "token_times_ns": token_times_ns,
            "token_wall_times_ns": token_wall_times_ns,
            "token_hash": hashlib.sha256(
                json.dumps(token_ids, separators=(",", ":")).encode("ascii")
            ).hexdigest(),
            "send_ns": send_ns,
            "send_wall_ns": send_wall_ns,
            "first_token_ns": token_times_ns[0] if token_times_ns else None,
            "last_token_ns": token_times_ns[-1] if token_times_ns else None,
            "first_token_wall_ns": token_wall_times_ns[0] if token_wall_times_ns else None,
            "last_token_wall_ns": token_wall_times_ns[-1] if token_wall_times_ns else None,
            "finish_ns": finish_ns,
            "completed": completed,
            "timings": timings,
            "error": error,
        }
        with self.condition:
            self.results[request_id] = result
            self.condition.notify_all()
        self.events.write({"type": "request_end", "monotonic_ns": finish_ns, **result})

    def run_symmetric(self):
        threads = []
        now_ns = time.monotonic_ns()
        now_wall_ns = time.time_ns()
        start_ns = now_ns + int(self.args.lead_sec * 1_000_000_000)
        start_wall_ns = now_wall_ns + (start_ns - now_ns)
        self.events.write(
            {
                "type": "workload_start",
                "scenario": "symmetric",
                "monotonic_ns": start_ns,
                "wall_time_ns": start_wall_ns,
                "incumbent_model": self.args.incumbent_model,
                "injected_model": self.args.injected_model,
            }
        )

        endpoints = (
            ("incumbent", self.args.incumbent_model, self.args.incumbent_url),
            ("injected", self.args.injected_model, self.args.injected_url),
        )
        for sequence, prompt in enumerate(self.prompts):
            release_ns = start_ns + int(
                sequence * self.args.arrival_gap_sec * 1_000_000_000
            )
            for role, model, base_url in endpoints:
                thread = threading.Thread(
                    target=self.run_request,
                    args=(role, model, base_url, prompt, sequence, release_ns),
                    daemon=True,
                )
                thread.start()
                threads.append(thread)

        self.events.write(
            {
                "type": "symmetric_release",
                "monotonic_ns": start_ns,
                "wall_time_ns": start_wall_ns,
                "request_count": len(threads),
            }
        )

        join_deadline = time.monotonic() + self.args.request_timeout + 30
        for thread in threads:
            thread.join(timeout=max(0.0, join_deadline - time.monotonic()))

        alive_threads = sum(thread.is_alive() for thread in threads)
        requests = sorted(self.results.values(), key=lambda item: item["send_ns"])
        expected_requests = 2 * len(self.prompts)
        by_key = {(result["role"], result["prompt_id"]): result for result in requests}
        expected_keys = {
            (role, prompt["id"])
            for role in ("incumbent", "injected")
            for prompt in self.prompts
        }

        pair_start_skews_ms = {}
        pair_completion_skews_ms = {}
        release_lateness_ms = []
        for prompt in self.prompts:
            incumbent = by_key.get(("incumbent", prompt["id"]))
            injected = by_key.get(("injected", prompt["id"]))
            if not incumbent or not injected:
                continue
            pair_start_skews_ms[prompt["id"]] = abs(
                incumbent["send_ns"] - injected["send_ns"]
            ) / 1_000_000.0
            incumbent_end = incumbent.get("last_token_ns")
            injected_end = injected.get("last_token_ns")
            if incumbent_end is not None and injected_end is not None:
                pair_completion_skews_ms[prompt["id"]] = abs(
                    incumbent_end - injected_end
                ) / 1_000_000.0
        for request in requests:
            release_lateness_ms.append(
                (request["send_ns"] - request["scheduled_release_ns"]) / 1_000_000.0
            )

        max_pair_start_skew_ms = max(pair_start_skews_ms.values(), default=None)
        release_alignment_spread_ms = (
            max(release_lateness_ms) - min(release_lateness_ms)
            if release_lateness_ms
            else None
        )
        start_alignment_ok = bool(
            len(pair_start_skews_ms) == len(self.prompts)
            and max_pair_start_skew_ms is not None
            and max_pair_start_skew_ms <= self.args.max_start_skew_ms
            and release_alignment_spread_ms is not None
            and release_alignment_spread_ms <= self.args.max_start_skew_ms
        )
        symmetric_input_ok = all(
            len(self.prompt_tokens[self.args.incumbent_model][prompt["id"]])
            == len(self.prompt_tokens[self.args.injected_model][prompt["id"]])
            for prompt in self.prompts
        )
        complete = bool(
            alive_threads == 0
            and len(requests) == expected_requests
            and set(by_key) == expected_keys
            and all(
                result["completed"]
                and not result["error"]
                and result["output_tokens"] == self.args.output_tokens
                for result in requests
            )
        )
        within_run_deterministic = set(by_key) == expected_keys and all(
            by_key[("incumbent", prompt["id"])]["token_hash"]
            == by_key[("injected", prompt["id"])]["token_hash"]
            for prompt in self.prompts
        )

        model_start_requests = {
            model: min(
                (result for result in requests if result["model"] == model),
                key=lambda result: result["send_ns"],
                default=None,
            )
            for model in (self.args.incumbent_model, self.args.injected_model)
        }
        overlap_start_request = max(
            (result for result in model_start_requests.values() if result is not None),
            key=lambda result: result["send_ns"],
            default=None,
        )
        model_ends = {
            model: max(
                (
                    result["last_token_ns"]
                    for result in requests
                    if result["model"] == model and result.get("last_token_ns") is not None
                ),
                default=None,
            )
            for model in (self.args.incumbent_model, self.args.injected_model)
        }
        model_completion_skew_ms = (
            abs(
                model_ends[self.args.incumbent_model]
                - model_ends[self.args.injected_model]
            )
            / 1_000_000.0
            if all(value is not None for value in model_ends.values())
            else None
        )
        valid = bool(complete and start_alignment_ok and symmetric_input_ok)
        summary = {
            "scenario": "symmetric",
            "valid": valid,
            "complete": complete,
            "start_alignment_ok": start_alignment_ok,
            "max_start_skew_limit_ms": self.args.max_start_skew_ms,
            "max_pair_start_skew_ms": max_pair_start_skew_ms,
            "release_alignment_spread_ms": release_alignment_spread_ms,
            "pair_start_skews_ms": pair_start_skews_ms,
            "pair_completion_skews_ms": pair_completion_skews_ms,
            "max_pair_completion_skew_ms": max(
                pair_completion_skews_ms.values(), default=None
            ),
            "model_completion_skew_ms": model_completion_skew_ms,
            "symmetric_input_ok": symmetric_input_ok,
            # Kept for compatibility with the existing report schema.
            "milestone_ok": None,
            "milestone_order_ok": start_alignment_ok,
            "injection_hook": {
                "requested": False,
                "ok": True,
                "return_code": 0,
                "duration_ms": 0.0,
                "stdout": "",
                "stderr": "",
                "error": "",
            },
            "within_run_deterministic": within_run_deterministic,
            "alive_threads": alive_threads,
            "workload_start_ns": start_ns,
            "workload_start_wall_ns": start_wall_ns,
            "injection_ns": (
                overlap_start_request["send_ns"] if overlap_start_request else None
            ),
            "injection_wall_ns": (
                overlap_start_request["send_wall_ns"] if overlap_start_request else None
            ),
            "exact_milestone_ns": None,
            "incumbent_model": self.args.incumbent_model,
            "injected_model": self.args.injected_model,
            "output_tokens_per_request": self.args.output_tokens,
            "request_count_per_model": len(self.prompts),
            "total_concurrency": 2 * len(self.prompts),
            "injection_token": None,
            "arrival_gap_sec": self.args.arrival_gap_sec,
            "prompt_tokens": {
                model: {prompt_id: len(tokens) for prompt_id, tokens in values.items()}
                for model, values in self.prompt_tokens.items()
            },
            "requests": requests,
        }
        Path(self.args.output).write_text(json.dumps(summary, indent=2) + "\n")
        self.events.close()
        return valid

    def incumbent_milestone_ready(self):
        return all(
            self.token_counts.get(f"incumbent-{prompt['id']}", 0) >= self.args.injection_token
            for prompt in self.prompts
        )

    def incumbent_failed(self):
        for prompt in self.prompts:
            request_id = f"incumbent-{prompt['id']}"
            result = self.results.get(request_id)
            if result and self.token_counts.get(request_id, 0) < self.args.injection_token:
                return True
        return False

    def run_injection_hook(self):
        result = {
            "requested": bool(self.args.injection_hook),
            "ok": True,
            "return_code": 0,
            "duration_ms": 0.0,
            "stdout": "",
            "stderr": "",
            "error": "",
        }
        if not self.args.injection_hook:
            return result

        env = os.environ.copy()
        env.update(
            {
                "INCUMBENT_MODEL": self.args.incumbent_model,
                "INJECTED_MODEL": self.args.injected_model,
                "INCUMBENT_TEAM": "0" if self.args.incumbent_model == "A" else "1",
                "INJECTED_TEAM": "0" if self.args.injected_model == "A" else "1",
            }
        )
        started_ns = time.monotonic_ns()
        try:
            completed = subprocess.run(
                ["sh", "-c", self.args.injection_hook],
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=self.args.injection_hook_timeout,
            )
            result["return_code"] = completed.returncode
            result["stdout"] = completed.stdout[-4096:]
            result["stderr"] = completed.stderr[-4096:]
            result["ok"] = completed.returncode == 0
        except (OSError, subprocess.SubprocessError) as exc:
            result["ok"] = False
            result["return_code"] = None
            result["error"] = f"{type(exc).__name__}: {exc}"
        result["duration_ms"] = (time.monotonic_ns() - started_ns) / 1_000_000
        self.events.write({"type": "injection_hook", **result})
        return result

    def run(self):
        if self.args.scenario == "symmetric":
            return self.run_symmetric()

        threads = []
        start_ns = time.monotonic_ns() + int(self.args.lead_sec * 1_000_000_000)
        self.events.write(
            {
                "type": "workload_start",
                "monotonic_ns": start_ns,
                "wall_time_ns": time.time_ns(),
                "incumbent_model": self.args.incumbent_model,
                "injected_model": self.args.injected_model,
            }
        )

        for sequence, prompt in enumerate(self.prompts):
            release_ns = start_ns + int(sequence * self.args.arrival_gap_sec * 1_000_000_000)
            thread = threading.Thread(
                target=self.run_request,
                args=(
                    "incumbent",
                    self.args.incumbent_model,
                    self.args.incumbent_url,
                    prompt,
                    sequence,
                    release_ns,
                ),
                daemon=True,
            )
            thread.start()
            threads.append(thread)

        milestone_deadline = time.monotonic() + self.args.milestone_timeout
        with self.condition:
            while not self.incumbent_milestone_ready():
                if self.incumbent_failed():
                    break
                remaining = milestone_deadline - time.monotonic()
                if remaining <= 0:
                    break
                self.condition.wait(timeout=min(remaining, 1.0))

        milestone_ok = self.incumbent_milestone_ready()
        hook_result = self.run_injection_hook() if milestone_ok else {
            "requested": bool(self.args.injection_hook),
            "ok": not self.args.injection_hook,
            "return_code": None,
            "duration_ms": 0.0,
            "stdout": "",
            "stderr": "",
            "error": "milestone not reached",
        }
        injection_ns = time.monotonic_ns()
        injection_wall_ns = time.time_ns()
        if milestone_ok:
            milestone_times = []
            for prompt in self.prompts:
                result_id = f"incumbent-{prompt['id']}"
                # The event stream is authoritative; this approximation is replaced
                # with exact per-request token timestamps after threads complete.
                milestone_times.append(result_id)
            self.events.write(
                {
                    "type": "injection_release",
                    "monotonic_ns": injection_ns,
                    "wall_time_ns": injection_wall_ns,
                    "milestone_token": self.args.injection_token,
                    "incumbent_requests": milestone_times,
                }
            )
            for sequence, prompt in enumerate(self.prompts):
                release_ns = injection_ns + int(sequence * self.args.arrival_gap_sec * 1_000_000_000)
                thread = threading.Thread(
                    target=self.run_request,
                    args=(
                        "injected",
                        self.args.injected_model,
                        self.args.injected_url,
                        prompt,
                        sequence,
                        release_ns,
                    ),
                    daemon=True,
                )
                thread.start()
                threads.append(thread)

        join_deadline = time.monotonic() + self.args.request_timeout + 30
        for thread in threads:
            thread.join(timeout=max(0.0, join_deadline - time.monotonic()))

        alive_threads = sum(thread.is_alive() for thread in threads)
        requests = sorted(self.results.values(), key=lambda item: item["send_ns"])
        expected_requests = 2 * len(self.prompts)
        exact_milestone_ns = None
        if milestone_ok and len(requests) == expected_requests:
            exact_milestone_ns = max(
                result["token_times_ns"][self.args.injection_token - 1]
                for result in requests
                if result["role"] == "incumbent"
            )

        complete = (
            milestone_ok
            and alive_threads == 0
            and len(requests) == expected_requests
            and all(
                result["completed"]
                and not result["error"]
                and result["output_tokens"] == self.args.output_tokens
                for result in requests
            )
        )
        milestone_order_ok = bool(
            exact_milestone_ns is not None
            and injection_ns >= exact_milestone_ns
            and all(
                result["send_ns"] >= injection_ns
                for result in requests
                if result["role"] == "injected"
            )
        )

        by_key = {(result["role"], result["prompt_id"]): result for result in requests}
        expected_keys = {
            (role, prompt["id"])
            for role in ("incumbent", "injected")
            for prompt in self.prompts
        }
        within_run_deterministic = set(by_key) == expected_keys and all(
            by_key[("incumbent", prompt["id"])]["token_hash"]
            == by_key[("injected", prompt["id"])]["token_hash"]
            for prompt in self.prompts
        )
        valid = complete and milestone_order_ok and hook_result["ok"]

        summary = {
            "scenario": "asynchronous",
            "valid": valid,
            "complete": complete,
            "milestone_ok": milestone_ok,
            "milestone_order_ok": milestone_order_ok,
            "injection_hook": hook_result,
            "within_run_deterministic": within_run_deterministic,
            "alive_threads": alive_threads,
            "workload_start_ns": start_ns,
            "injection_ns": injection_ns if milestone_ok else None,
            "injection_wall_ns": injection_wall_ns if milestone_ok else None,
            "exact_milestone_ns": exact_milestone_ns,
            "incumbent_model": self.args.incumbent_model,
            "injected_model": self.args.injected_model,
            "output_tokens_per_request": self.args.output_tokens,
            "request_count_per_model": len(self.prompts),
            "total_concurrency": 2 * len(self.prompts),
            "injection_token": self.args.injection_token,
            "arrival_gap_sec": self.args.arrival_gap_sec,
            "prompt_tokens": {
                model: {prompt_id: len(tokens) for prompt_id, tokens in values.items()}
                for model, values in self.prompt_tokens.items()
            },
            "requests": requests,
        }
        Path(self.args.output).write_text(json.dumps(summary, indent=2) + "\n")
        self.events.close()
        return valid


def validate_prompt_manifest(prompts):
    if not isinstance(prompts, list) or not 1 <= len(prompts) <= 4:
        raise ValueError("the prompt manifest must contain between one and four prompts")
    prompt_ids = [prompt.get("id") for prompt in prompts if isinstance(prompt, dict)]
    if len(prompt_ids) != len(prompts) or any(not prompt_id for prompt_id in prompt_ids):
        raise ValueError("every prompt must have a unique non-empty id")
    if len(set(prompt_ids)) != len(prompt_ids):
        raise ValueError("every prompt must have a unique non-empty id")
    if any(not isinstance(prompt.get("prompt"), str) or not prompt["prompt"] for prompt in prompts):
        raise ValueError("every prompt must contain non-empty text")
    targets = [prompt.get("target_tokens") for prompt in prompts]
    if any(not isinstance(target, int) or target <= 0 for target in targets):
        raise ValueError("every prompt must define a positive target_tokens value")
    if len(set(targets)) != 1:
        raise ValueError("all prompts must use the same target_tokens value")


def build_equal_prompt_tokens(prompts, source_tokens):
    """Pad distinct tokenized prompts to one exact length for each model."""
    target_tokens = prompts[0]["target_tokens"]
    result = {}
    for model, model_tokens in source_tokens.items():
        result[model] = {}
        for prompt in prompts:
            prompt_id = prompt["id"]
            tokens = list(model_tokens[prompt_id])
            if not tokens:
                raise ValueError(f"prompt {prompt_id} tokenized to an empty sequence on model {model}")
            if len(tokens) > target_tokens:
                raise ValueError(
                    f"prompt {prompt_id} has {len(tokens)} tokens on model {model}, "
                    f"exceeding target_tokens={target_tokens}"
                )
            tokens.extend([tokens[-1]] * (target_tokens - len(tokens)))
            result[model][prompt_id] = tokens

    lengths = {
        len(tokens)
        for model_tokens in result.values()
        for tokens in model_tokens.values()
    }
    if lengths != {target_tokens}:
        raise ValueError(f"equal-length prompt construction failed: {sorted(lengths)}")
    return result


def select_prompts(prompts, request_count):
    if not 1 <= request_count <= len(prompts):
        raise ValueError(
            f"request count must be between 1 and {len(prompts)}"
        )
    return prompts[:request_count]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--scenario", choices=("asynchronous", "symmetric"), default="asynchronous"
    )
    parser.add_argument("--incumbent-url", required=True)
    parser.add_argument("--injected-url", required=True)
    parser.add_argument("--incumbent-model", choices=("A", "B"), required=True)
    parser.add_argument("--injected-model", choices=("A", "B"), required=True)
    parser.add_argument("--prompts", required=True)
    parser.add_argument("--request-count", type=int, default=4)
    parser.add_argument("--events", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--output-tokens", type=int, default=256)
    parser.add_argument("--injection-token", type=int, default=4)
    parser.add_argument("--arrival-gap-sec", type=float)
    parser.add_argument("--lead-sec", type=float)
    parser.add_argument("--max-start-skew-ms", type=float, default=25.0)
    parser.add_argument("--seed", type=int, default=1234)
    parser.add_argument("--request-timeout", type=float, default=1800.0)
    parser.add_argument("--milestone-timeout", type=float, default=600.0)
    parser.add_argument("--injection-hook", default="")
    parser.add_argument("--injection-hook-timeout", type=float, default=30.0)
    args = parser.parse_args()

    if args.arrival_gap_sec is None:
        args.arrival_gap_sec = 0.0 if args.scenario == "symmetric" else 1.0
    if args.lead_sec is None:
        args.lead_sec = 2.0 if args.scenario == "symmetric" else 1.0

    if (
        args.scenario == "asynchronous"
        and (args.injection_token <= 0 or args.injection_token >= args.output_tokens)
    ):
        parser.error("--injection-token must be between 1 and output-tokens - 1")
    if args.scenario == "symmetric" and args.injection_hook:
        parser.error("--injection-hook is only valid for the asynchronous scenario")
    if args.arrival_gap_sec < 0 or args.lead_sec <= 0 or args.max_start_skew_ms <= 0:
        parser.error("arrival gap must be non-negative; lead and skew limit must be positive")
    if args.incumbent_model == args.injected_model:
        parser.error("incumbent and injected model labels must differ")

    prompts = json.loads(Path(args.prompts).read_text())
    try:
        validate_prompt_manifest(prompts)
    except ValueError as exc:
        parser.error(str(exc))

    model_urls = {
        args.incumbent_model: args.incumbent_url,
        args.injected_model: args.injected_url,
    }
    source_tokens = {}
    for model, base_url in model_urls.items():
        source_tokens[model] = {}
        for prompt in prompts:
            tokens = tokenize(base_url, prompt["prompt"], args.request_timeout)
            source_tokens[model][prompt["id"]] = tokens

    try:
        prompt_tokens = build_equal_prompt_tokens(prompts, source_tokens)
        prompts = select_prompts(prompts, args.request_count)
    except ValueError as exc:
        parser.error(str(exc))

    selected_ids = {prompt["id"] for prompt in prompts}
    prompt_tokens = {
        model: {
            prompt_id: tokens
            for prompt_id, tokens in model_tokens.items()
            if prompt_id in selected_ids
        }
        for model, model_tokens in prompt_tokens.items()
    }

    valid = Workload(args, prompts, prompt_tokens).run()
    raise SystemExit(0 if valid else 2)


if __name__ == "__main__":
    main()
