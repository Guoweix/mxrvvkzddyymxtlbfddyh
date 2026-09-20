#!/usr/bin/env python3
"""Synchronized first-token phase gate for the legacy benchmark."""

import argparse
import json
import threading
import time
import urllib.request
from pathlib import Path


def request_first_token(url, prompt, tokens, target, timeout, result, index, barrier):
    item = {"slot": index, "first_token_ms": None, "error": None}
    try:
        barrier.wait(timeout=max(1.0, target - time.monotonic() + 10.0))
        while time.monotonic() < target:
            remaining = target - time.monotonic()
            if remaining > 0.002:
                time.sleep(remaining - 0.002)
        payload = {
            "prompt": prompt,
            "n_predict": tokens,
            "temperature": 0,
            "stream": True,
        }
        request = urllib.request.Request(
            url,
            data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(request, timeout=timeout) as response:
            for raw_line in response:
                if not raw_line.startswith(b"data:"):
                    continue
                data = raw_line[5:].strip()
                if not data or data == b"[DONE]":
                    continue
                try:
                    chunk = json.loads(data)
                except json.JSONDecodeError:
                    continue
                if chunk.get("tokens"):
                    item["first_token_ms"] = (time.monotonic() - target) * 1000.0
                    break
    except Exception as exc:  # the result file is the diagnostic artifact
        item["error"] = str(exc)
    result[index] = item


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", required=True)
    parser.add_argument("--prompt", required=True)
    parser.add_argument("--concurrency", type=int, required=True)
    parser.add_argument("--tokens", type=int, default=1)
    parser.add_argument("--threshold-ms", type=float, required=True)
    parser.add_argument("--lead-sec", type=float, default=3.0)
    parser.add_argument("--timeout-sec", type=float, default=60.0)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    target = time.monotonic() + args.lead_sec
    barrier = threading.Barrier(args.concurrency)
    results = {}
    threads = [
        threading.Thread(
            target=request_first_token,
            args=(
                args.url,
                args.prompt,
                args.tokens,
                target,
                args.timeout_sec,
                results,
                index,
                barrier,
            ),
            daemon=True,
        )
        for index in range(args.concurrency)
    ]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(args.timeout_sec + args.lead_sec + 5.0)

    items = [results.get(index, {"slot": index, "first_token_ms": None, "error": "thread timeout"}) for index in range(args.concurrency)]
    values = [item["first_token_ms"] for item in items if item["first_token_ms"] is not None]
    spread = max(values) - min(values) if len(values) == args.concurrency else None
    report = {
        "url": args.url,
        "concurrency": args.concurrency,
        "tokens": args.tokens,
        "threshold_ms": args.threshold_ms,
        "lead_sec": args.lead_sec,
        "release_target_monotonic": target,
        "first_token_ms": items,
        "spread_ms": spread,
        "passed": spread is not None and spread <= args.threshold_ms,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, separators=(",", ":")))
    return 0 if report["passed"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
