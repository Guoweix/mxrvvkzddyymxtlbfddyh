#!/usr/bin/env python3
"""llama-server lifecycle for the CFS-only benchmark."""

from __future__ import annotations

import concurrent.futures
import json
import shlex

from .board import http_json, start_background, stop_pid, wait_health


class ServerManager:
    def __init__(self, remote, args, remote_dir):
        self.remote = remote
        self.args = args
        self.remote_dir = remote_dir
        self.pids = {}

    def _argv(self, port, model_path, threads, threads_batch, requests, overrides):
        base = self.args.llm_path
        runtime_bin = overrides.get("runtime_bin") or self.args.runtime_bin_dir or f"{base}/bin"
        if overrides.get("direct"):
            return [
                f"{runtime_bin}/llama-server",
                "-m",
                model_path,
                "--host",
                "0.0.0.0",
                "--port",
                str(port),
                "-ngl",
                "0",
                "-t",
                str(threads),
                "-tb",
                str(threads_batch),
                "-c",
                str(self.args.context),
                "-np",
                str(requests),
                "--cache-ram",
                "0",
                *overrides.get("extra_args", ()),
            ]
        loader = overrides.get("loader") or f"{base}/bin/ld-linux-riscv64-lp64d.so.1"
        library_path = overrides.get("library_path") or (
            f"{runtime_bin}:{base}/bin" if runtime_bin != f"{base}/bin" else runtime_bin
        )
        return [
            loader,
            "--library-path",
            library_path,
            f"{runtime_bin}/llama-server",
            "-m",
            model_path,
            "--host",
            "0.0.0.0",
            "--port",
            str(port),
            "-ngl",
            "0",
            "-t",
            str(threads),
            "-tb",
            str(threads_batch),
            "-c",
            str(self.args.context),
            "-np",
            str(requests),
            "--cache-ram",
            "0",
            *overrides.get("extra_args", ()),
        ]

    def start(
        self,
        model,
        port,
        model_path,
        threads,
        threads_batch,
        requests,
        run_tag,
        cpu_mask=None,
        overrides=None,
    ):
        overrides = overrides or {}
        argv = self._argv(port, model_path, threads, threads_batch, requests, overrides)
        affinity_code = ""
        if cpu_mask is not None:
            affinity_code = f"os.sched_setaffinity(0, {int(cpu_mask)})\n"
        env = dict(overrides.get("extra_env", {}))
        if overrides.get("direct"):
            env["LD_LIBRARY_PATH"] = overrides.get("library_path") or overrides["runtime_bin"]
        env_code = "".join(
            f"os.environ[{key!r}]={value!r}\n" for key, value in env.items()
        )
        code = (
            "import json,os\n"
            + affinity_code
            + env_code
            + f"argv=json.loads({json.dumps(json.dumps(argv))})\n"
            + "os.execv(argv[0],argv)\n"
        )
        command = f"python3 -c {shlex.quote(code)}"
        log_path = f"{self.remote_dir}/{run_tag}_server_{model.lower()}.log"
        pid = start_background(self.remote, command, log_path)
        self.pids[model] = pid
        wait_health(f"http://{self.args.host}:{port}", self.args.server_start_timeout)
        print(
            f"[INFO] server {model}: pid={pid}, port={port}, "
            f"threads={threads}/{threads_batch}, cpu_mask={cpu_mask}"
        )
        return pid

    def stop(self, model):
        stop_pid(self.remote, self.pids.pop(model, None))

    def stop_all(self):
        for model in list(self.pids):
            self.stop(model)

    def warmup(self, servers):
        """servers: list of dicts with keys port, requests, label."""

        def one(server, slot):
            payload = {
                "prompt": f"benchmark deterministic warmup {server['label']} slot {slot}",
                "n_predict": self.args.warmup_tokens,
                "temperature": 0,
                "top_k": 1,
                "top_p": 1.0,
                "seed": self.args.seed,
                "ignore_eos": True,
                "cache_prompt": False,
                "stream": False,
            }
            response = http_json(
                f"http://{self.args.host}:{server['port']}/completion",
                payload,
                timeout=self.args.server_start_timeout,
            )
            if int(response.get("tokens_predicted", 0)) != self.args.warmup_tokens:
                raise RuntimeError(f"incomplete warmup for {server['label']} slot {slot}")

        futures = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            for server in servers:
                for slot in range(server["requests"]):
                    futures.append(pool.submit(one, server, slot))
            for future in futures:
                future.result()
        if self.args.quiet_sec:
            import time

            time.sleep(self.args.quiet_sec)
