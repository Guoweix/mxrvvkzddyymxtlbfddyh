#!/usr/bin/env python3
"""Deterministic streaming workloads for the dual and multi llama scenarios.

This module is executed as its own process so request timing is isolated from
the SSH/SCP orchestration. It supports two modes:

``dual``
    Two servers with named roles. ``--scenario asynchronous`` releases the
    incumbent requests first and injects the second server once every incumbent
    request reached ``--injection-token``; ``--scenario synchronized`` releases
    every request at one shared absolute time (the lenient simultaneous case).

``multi``
    One or more servers, all requests released at one shared absolute time.
"""

from __future__ import annotations

import argparse
import hashlib
import json
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


class WorkloadEngine:
    """Runs streaming requests and records per-token events and results."""

    def __init__(self, args, events_path):
        self.args = args
        self.events = EventWriter(events_path)
        self.condition = threading.Condition()
        self.token_counts = {}
        self.results = {}
        self.threads = []

    def _record_token(self, request_id):
        with self.condition:
            self.token_counts[request_id] = self.token_counts.get(request_id, 0) + 1
            self.condition.notify_all()

    def execute_request(self, spec):
        role = spec["role"]
        model = spec["model"]
        base_url = spec["base_url"]
        prompt_id = spec["prompt_id"]
        sequence = spec["sequence"]
        release_ns = spec["release_ns"]
        tokens = spec["tokens"]
        request_id = f"{role}-{prompt_id}"

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
                "prompt_id": prompt_id,
                "sequence": sequence,
                "scheduled_release_ns": release_ns,
                "monotonic_ns": send_ns,
                "wall_time_ns": send_wall_ns,
            }
        )

        payload = {
            "prompt": tokens,
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
                        chunk_text = chunk.get("content")
                        if not isinstance(chunk_text, str):
                            chunk_text = ""
                        for chunk_offset, token_id in enumerate(chunk_tokens):
                            token_index = len(token_ids) + 1
                            token_ids.append(int(token_id))
                            token_times_ns.append(chunk_ns)
                            token_wall_times_ns.append(chunk_wall_ns)
                            self.events.write(
                                {
                                    "type": "token",
                                    "request_id": request_id,
                                    "role": role,
                                    "model": model,
                                    "prompt_id": prompt_id,
                                    "token_index": token_index,
                                    "token_id": int(token_id),
                                    "text": chunk_text if chunk_offset == 0 else "",
                                    "monotonic_ns": chunk_ns,
                                    "wall_time_ns": chunk_wall_ns,
                                }
                            )
                            self._record_token(request_id)

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
            "prompt_id": prompt_id,
            "sequence": sequence,
            "scheduled_release_ns": release_ns,
            "input_tokens": len(tokens),
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
        return result

    def start(self, spec):
        thread = threading.Thread(target=self.execute_request, args=(spec,), daemon=True)
        thread.start()
        self.threads.append(thread)
        return thread

    def join_all(self):
        deadline = time.monotonic() + self.args.request_timeout + 30
        for thread in self.threads:
            thread.join(timeout=max(0.0, deadline - time.monotonic()))
        return sum(thread.is_alive() for thread in self.threads)

    def writes(self):
        self.events.close()

    def milestone_ready(self, request_ids, token):
        return all(self.token_counts.get(request_id, 0) >= token for request_id in request_ids)

    def milestone_failed(self, request_ids, token):
        for request_id in request_ids:
            result = self.results.get(request_id)
            if result and self.token_counts.get(request_id, 0) < token:
                return True
        return False


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
                raise ValueError(f"prompt {prompt_id} tokenized empty on model {model}")
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
        raise ValueError(f"request count must be between 1 and {len(prompts)}")
    return prompts[:request_count]


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


def run_dual(args):
    prompts = json.loads(Path(args.prompts).read_text())
    validate_prompt_manifest(prompts)

    model_urls = {
        args.incumbent_model: args.incumbent_url,
        args.injected_model: args.injected_url,
    }
    source_tokens = {}
    for model, base_url in model_urls.items():
        source_tokens[model] = {
            prompt["id"]: tokenize(base_url, prompt["prompt"], args.request_timeout)
            for prompt in prompts
        }
    prompt_tokens = build_equal_prompt_tokens(prompts, source_tokens)
    prompts = select_prompts(prompts, args.request_count)
    selected_ids = {prompt["id"] for prompt in prompts}
    prompt_tokens = {
        model: {pid: toks for pid, toks in model_tokens.items() if pid in selected_ids}
        for model, model_tokens in prompt_tokens.items()
    }

    engine = WorkloadEngine(args, args.events)
    try:
        if args.scenario == "asynchronous":
            return _dual_asynchronous(args, engine, prompts, prompt_tokens)
        return _dual_synchronized(args, engine, prompts, prompt_tokens)
    finally:
        engine.writes()


def _dual_asynchronous(args, engine, prompts, prompt_tokens):
    start_ns = time.monotonic_ns() + int(args.lead_sec * 1_000_000_000)
    engine.events.write(
        {
            "type": "workload_start",
            "scenario": "asynchronous",
            "monotonic_ns": start_ns,
            "wall_time_ns": time.time_ns(),
            "incumbent_model": args.incumbent_model,
            "injected_model": args.injected_model,
        }
    )

    incumbent_ids = []
    for sequence, prompt in enumerate(prompts):
        release_ns = start_ns + int(sequence * args.arrival_gap_sec * 1_000_000_000)
        request_id = f"incumbent-{prompt['id']}"
        incumbent_ids.append(request_id)
        engine.start(
            {
                "role": "incumbent",
                "model": args.incumbent_model,
                "base_url": args.incumbent_url,
                "prompt_id": prompt["id"],
                "sequence": sequence,
                "release_ns": release_ns,
                "tokens": prompt_tokens[args.incumbent_model][prompt["id"]],
            }
        )

    milestone_deadline = time.monotonic() + args.milestone_timeout
    with engine.condition:
        while not engine.milestone_ready(incumbent_ids, args.injection_token):
            if engine.milestone_failed(incumbent_ids, args.injection_token):
                break
            remaining = milestone_deadline - time.monotonic()
            if remaining <= 0:
                break
            engine.condition.wait(timeout=min(remaining, 1.0))

    milestone_ok = engine.milestone_ready(incumbent_ids, args.injection_token)
    injection_ns = time.monotonic_ns()
    injection_wall_ns = time.time_ns()
    if milestone_ok:
        engine.events.write(
            {
                "type": "injection_release",
                "monotonic_ns": injection_ns,
                "wall_time_ns": injection_wall_ns,
                "milestone_token": args.injection_token,
                "incumbent_requests": incumbent_ids,
            }
        )
        for sequence, prompt in enumerate(prompts):
            release_ns = injection_ns + int(sequence * args.arrival_gap_sec * 1_000_000_000)
            engine.start(
                {
                    "role": "injected",
                    "model": args.injected_model,
                    "base_url": args.injected_url,
                    "prompt_id": prompt["id"],
                    "sequence": sequence,
                    "release_ns": release_ns,
                    "tokens": prompt_tokens[args.injected_model][prompt["id"]],
                }
            )

    alive_threads = engine.join_all()
    requests = sorted(engine.results.values(), key=lambda item: item["send_ns"])
    expected_requests = 2 * len(prompts)

    exact_milestone_ns = None
    if milestone_ok and len(requests) == expected_requests:
        exact_milestone_ns = max(
            result["token_times_ns"][args.injection_token - 1]
            for result in requests
            if result["role"] == "incumbent"
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
    complete = bool(
        milestone_ok
        and alive_threads == 0
        and len(requests) == expected_requests
        and all(
            result["completed"]
            and not result["error"]
            and result["output_tokens"] == args.output_tokens
            for result in requests
        )
    )
    within_run_deterministic = _within_run_deterministic(
        requests, (args.incumbent_model, args.injected_model), prompts
    )
    valid = complete and milestone_order_ok

    summary = {
        "scenario": "asynchronous",
        "valid": valid,
        "complete": complete,
        "milestone_ok": milestone_ok,
        "milestone_order_ok": milestone_order_ok,
        "within_run_deterministic": within_run_deterministic,
        "alive_threads": alive_threads,
        "workload_start_ns": start_ns,
        "injection_ns": injection_ns if milestone_ok else None,
        "injection_wall_ns": injection_wall_ns if milestone_ok else None,
        "exact_milestone_ns": exact_milestone_ns,
        "incumbent_model": args.incumbent_model,
        "injected_model": args.injected_model,
        "output_tokens_per_request": args.output_tokens,
        "request_count_per_model": len(prompts),
        "total_concurrency": 2 * len(prompts),
        "injection_token": args.injection_token,
        "arrival_gap_sec": args.arrival_gap_sec,
        "prompt_tokens": _prompt_lengths(prompt_tokens),
        "requests": requests,
    }
    _write_summary(args.output, summary)
    return valid


def _dual_synchronized(args, engine, prompts, prompt_tokens):
    now_ns = time.monotonic_ns()
    now_wall_ns = time.time_ns()
    start_ns = now_ns + int(args.lead_sec * 1_000_000_000)
    start_wall_ns = now_wall_ns + (start_ns - now_ns)
    engine.events.write(
        {
            "type": "workload_start",
            "scenario": "synchronized",
            "monotonic_ns": start_ns,
            "wall_time_ns": start_wall_ns,
            "incumbent_model": args.incumbent_model,
            "injected_model": args.injected_model,
        }
    )

    endpoints = (
        ("incumbent", args.incumbent_model, args.incumbent_url),
        ("injected", args.injected_model, args.injected_url),
    )
    for sequence, prompt in enumerate(prompts):
        release_ns = start_ns + int(sequence * args.arrival_gap_sec * 1_000_000_000)
        for role, model, base_url in endpoints:
            engine.start(
                {
                    "role": role,
                    "model": model,
                    "base_url": base_url,
                    "prompt_id": prompt["id"],
                    "sequence": sequence,
                    "release_ns": release_ns,
                    "tokens": prompt_tokens[model][prompt["id"]],
                }
            )

    engine.events.write(
        {
            "type": "synchronized_release",
            "monotonic_ns": start_ns,
            "wall_time_ns": start_wall_ns,
            "request_count": len(engine.threads),
        }
    )

    alive_threads = engine.join_all()
    requests = sorted(engine.results.values(), key=lambda item: item["send_ns"])
    expected_requests = 2 * len(prompts)
    by_key = {(result["role"], result["prompt_id"]): result for result in requests}
    expected_keys = {
        (role, prompt["id"])
        for role in ("incumbent", "injected")
        for prompt in prompts
    }

    release_lateness_ms = [
        (request["send_ns"] - request["scheduled_release_ns"]) / 1_000_000.0
        for request in requests
    ]
    release_alignment_spread_ms = (
        max(release_lateness_ms) - min(release_lateness_ms) if release_lateness_ms else None
    )
    complete = bool(
        alive_threads == 0
        and len(requests) == expected_requests
        and set(by_key) == expected_keys
        and all(
            result["completed"]
            and not result["error"]
            and result["output_tokens"] == args.output_tokens
            for result in requests
        )
    )
    symmetric_input_ok = all(
        len(prompt_tokens[args.incumbent_model][prompt["id"]])
        == len(prompt_tokens[args.injected_model][prompt["id"]])
        for prompt in prompts
    )
    within_run_deterministic = _within_run_deterministic(
        requests, (args.incumbent_model, args.injected_model), prompts
    )
    valid = complete and symmetric_input_ok

    summary = {
        "scenario": "synchronized",
        "valid": valid,
        "complete": complete,
        "symmetric_input_ok": symmetric_input_ok,
        "release_alignment_spread_ms": release_alignment_spread_ms,
        "within_run_deterministic": within_run_deterministic,
        "alive_threads": alive_threads,
        "workload_start_ns": start_ns,
        "workload_start_wall_ns": start_wall_ns,
        "injection_ns": None,
        "incumbent_model": args.incumbent_model,
        "injected_model": args.injected_model,
        "output_tokens_per_request": args.output_tokens,
        "request_count_per_model": len(prompts),
        "total_concurrency": 2 * len(prompts),
        "arrival_gap_sec": args.arrival_gap_sec,
        "prompt_tokens": _prompt_lengths(prompt_tokens),
        "requests": requests,
    }
    _write_summary(args.output, summary)
    return valid


def run_multi(args):
    endpoints = []
    for item in args.endpoint:
        model_id, _, rest = item.partition(":")
        url, _, count = rest.rpartition(":")
        endpoints.append((model_id, url, int(count)))

    prompts = json.loads(Path(args.prompts).read_text())
    validate_prompt_manifest(prompts)

    engine = WorkloadEngine(args, args.events)
    try:
        source_tokens = {}
        for model_id, url, _ in endpoints:
            source_tokens[model_id] = {
                prompt["id"]: tokenize(url, prompt["prompt"], args.request_timeout)
                for prompt in prompts
            }
        prompt_tokens = build_equal_prompt_tokens(prompts, source_tokens)

        start_ns = time.monotonic_ns() + int(args.lead_sec * 1_000_000_000)
        engine.events.write(
            {
                "type": "workload_start",
                "scenario": "multi",
                "monotonic_ns": start_ns,
                "wall_time_ns": time.time_ns(),
            }
        )
        for model_id, url, count in endpoints:
            if not 1 <= count <= len(prompts):
                raise ValueError(
                    f"model {model_id} requests={count} exceeds the {len(prompts)} prompts "
                    f"in the manifest"
                )
            for index, prompt in enumerate(select_prompts(prompts, count)):
                engine.start(
                    {
                        "role": model_id,
                        "model": model_id,
                        "base_url": url,
                        "prompt_id": prompt["id"],
                        "sequence": index,
                        "release_ns": start_ns,
                        "tokens": prompt_tokens[model_id][prompt["id"]],
                    }
                )

        alive_threads = engine.join_all()
        requests = sorted(engine.results.values(), key=lambda item: item["send_ns"])
        expected_requests = sum(count for _, _, count in endpoints)
        complete = bool(
            alive_threads == 0
            and len(requests) == expected_requests
            and all(
                result["completed"]
                and not result["error"]
                and result["output_tokens"] == args.output_tokens
                for result in requests
            )
        )
        release_lateness_ms = [
            (request["send_ns"] - request["scheduled_release_ns"]) / 1_000_000.0
            for request in requests
        ]
        summary = {
            "scenario": "multi",
            "valid": complete,
            "complete": complete,
            "alive_threads": alive_threads,
            "workload_start_ns": start_ns,
            "release_alignment_spread_ms": (
                max(release_lateness_ms) - min(release_lateness_ms)
                if release_lateness_ms
                else None
            ),
            "output_tokens_per_request": args.output_tokens,
            "total_concurrency": expected_requests,
            "prompt_tokens": _prompt_lengths(prompt_tokens),
            "requests": requests,
        }
        _write_summary(args.output, summary)
        return complete
    finally:
        engine.writes()


def _prompt_lengths(prompt_tokens):
    return {
        model: {prompt_id: len(tokens) for prompt_id, tokens in values.items()}
        for model, values in prompt_tokens.items()
    }


def _within_run_deterministic(requests, models, prompts):
    by_key = {(result["role"], result["prompt_id"]): result for result in requests}
    expected_keys = {
        (role, prompt["id"])
        for role in ("incumbent", "injected")
        for prompt in prompts
    }
    if set(by_key) != expected_keys:
        return False
    return all(
        by_key[("incumbent", prompt["id"])]["token_hash"]
        == by_key[("injected", prompt["id"])]["token_hash"]
        for prompt in prompts
    )


def _write_summary(path, summary):
    Path(path).write_text(json.dumps(summary, indent=2) + "\n")


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("dual", "multi"), required=True)
    parser.add_argument("--events", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--output-tokens", type=int, default=256)
    parser.add_argument("--lead-sec", type=float)
    parser.add_argument("--seed", type=int, default=1234)
    parser.add_argument("--request-timeout", type=float, default=3600.0)
    # dual
    parser.add_argument("--scenario", choices=("asynchronous", "synchronized"))
    parser.add_argument("--incumbent-url")
    parser.add_argument("--injected-url")
    parser.add_argument("--incumbent-model", choices=("A", "B"))
    parser.add_argument("--injected-model", choices=("A", "B"))
    parser.add_argument("--prompts")
    parser.add_argument("--request-count", type=int, default=4)
    parser.add_argument("--injection-token", type=int, default=4)
    parser.add_argument("--arrival-gap-sec", type=float)
    parser.add_argument("--milestone-timeout", type=float, default=1800.0)
    # multi
    parser.add_argument("--endpoint", action="append", default=[])
    return parser


def main():
    args = build_parser().parse_args()
    if args.lead_sec is None:
        args.lead_sec = 1.0 if args.mode == "dual" and args.scenario == "asynchronous" else 2.0

    if args.mode == "dual":
        for required in ("scenario", "incumbent_url", "injected_url", "incumbent_model", "injected_model", "prompts"):
            if getattr(args, required) in (None, ""):
                raise SystemExit(f"--{required.replace('_', '-')} is required for dual mode")
        if args.incumbent_model == args.injected_model:
            raise SystemExit("incumbent and injected model labels must differ")
        if args.arrival_gap_sec is None:
            args.arrival_gap_sec = 0.0 if args.scenario == "synchronized" else 1.0
        if args.scenario == "asynchronous" and not (
            0 < args.injection_token < args.output_tokens
        ):
            raise SystemExit("--injection-token must be between 1 and output-tokens - 1")
        if args.scenario == "synchronized" and args.arrival_gap_sec != 0:
            raise SystemExit("synchronized mode requires --arrival-gap-sec 0")
        valid = run_dual(args)
    else:
        if not args.endpoint:
            raise SystemExit("--endpoint is required for multi mode")
        if not args.prompts:
            raise SystemExit("--prompts is required for multi mode")
        valid = run_multi(args)

    raise SystemExit(0 if valid else 2)


if __name__ == "__main__":
    main()
