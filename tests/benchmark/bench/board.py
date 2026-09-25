#!/usr/bin/env python3
"""Remote board access: SSH/SCP transport, process control and health checks."""

from __future__ import annotations

import json
import shlex
import subprocess
import time
import urllib.error
import urllib.request


def http_json(url, payload=None, timeout=10):
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/json"} if data is not None else {},
        method="POST" if data is not None else "GET",
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.load(response)


def wait_health(url, timeout=180):
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            payload = http_json(f"{url}/health", timeout=5)
            if payload.get("status") in {"ok", "no slot available"}:
                return
        except (OSError, urllib.error.URLError, json.JSONDecodeError) as exc:
            last_error = exc
        time.sleep(1)
    raise RuntimeError(f"server health timeout at {url}: {last_error}")


def dmesg_delta(before, after):
    before_lines = before.splitlines()
    after_lines = after.splitlines()
    if not before_lines:
        return after
    last = before_lines[-1]
    for index in range(len(after_lines) - 1, -1, -1):
        if after_lines[index] == last:
            tail = after_lines[index + 1 :]
            return "\n".join(tail) + ("\n" if tail else "")
    return "# Unable to align dmesg snapshots; full post-run log follows.\n" + after


def start_background(remote, command, log_path):
    wrapped = f"nohup {command} > {shlex.quote(log_path)} 2>&1 < /dev/null & echo $!"
    result = remote.run(wrapped)
    pids = [int(line) for line in result.stdout.splitlines() if line.strip().isdigit()]
    if not pids:
        raise RuntimeError(f"failed to capture remote PID for: {command}")
    pid = pids[-1]
    time.sleep(0.2)
    if remote.run(f"kill -0 {pid}", check=False).returncode:
        log = remote.run(f"cat {shlex.quote(log_path)}", check=False).stdout
        raise RuntimeError(f"remote process {pid} exited during startup\n{log}")
    return pid


def stop_pid(remote, pid, grace=10):
    if not pid:
        return
    remote.run(f"kill -TERM {int(pid)} 2>/dev/null || true", check=False)
    deadline = time.monotonic() + grace
    while time.monotonic() < deadline:
        if remote.run(f"kill -0 {int(pid)} 2>/dev/null", check=False).returncode:
            return
        time.sleep(0.25)
    remote.run(f"kill -KILL {int(pid)} 2>/dev/null || true", check=False)


def pids_for_port(remote, port):
    code = (
        "import os\n"
        f"port={json.dumps(str(int(port)))}\n"
        "for name in os.listdir('/proc'):\n"
        "    if not name.isdigit(): continue\n"
        "    try:\n"
        "        args=open('/proc/'+name+'/cmdline','rb').read().rstrip(b'\\0').split(b'\\0')\n"
        "    except OSError: continue\n"
        "    decoded=[arg.decode(errors='replace') for arg in args]\n"
        "    if not any(arg.endswith('/llama-server') for arg in decoded): continue\n"
        "    if any(decoded[i]=='--port' and decoded[i+1]==port for i in range(len(decoded)-1)):\n"
        "        print(name)\n"
    )
    result = remote.run(f"python3 -c {shlex.quote(code)}", check=False)
    return [int(line) for line in result.stdout.splitlines() if line.strip().isdigit()]


def pids_with_cmdline_arg(remote, argument):
    code = (
        "import os\n"
        f"argument={json.dumps(str(argument))}\n"
        "for name in os.listdir('/proc'):\n"
        "    if not name.isdigit(): continue\n"
        "    try:\n"
        "        args=open('/proc/'+name+'/cmdline','rb').read().rstrip(b'\\0').split(b'\\0')\n"
        "    except OSError: continue\n"
        "    decoded=[arg.decode(errors='replace') for arg in args]\n"
        "    if argument in decoded: print(name)\n"
    )
    result = remote.run(f"python3 -c {shlex.quote(code)}", check=False)
    return [int(line) for line in result.stdout.splitlines() if line.strip().isdigit()]


class Remote:
    def __init__(self, host, user, password):
        self.host = host
        self.user = user
        self.password = password
        self.ssh_base = [
            "sshpass",
            "-p",
            password,
            "ssh",
            "-o",
            "StrictHostKeyChecking=no",
            f"{user}@{host}",
        ]
        self.scp_base = [
            "sshpass",
            "-p",
            password,
            "scp",
            "-o",
            "StrictHostKeyChecking=no",
        ]

    def run(self, command, check=True, timeout=None):
        result = subprocess.run(
            self.ssh_base + [command],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
        )
        if check and result.returncode:
            raise RuntimeError(
                f"remote command failed ({result.returncode}): {command}\n"
                f"{result.stderr.strip()}"
            )
        return result

    def upload(self, local_path, remote_path):
        subprocess.run(
            self.scp_base + [str(local_path), f"{self.user}@{self.host}:{remote_path}"],
            check=True,
            stdout=subprocess.DEVNULL,
        )

    def download(self, remote_path, local_path, optional=False):
        result = subprocess.run(
            self.scp_base + [f"{self.user}@{self.host}:{remote_path}", str(local_path)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL if optional else None,
        )
        if result.returncode and not optional:
            raise RuntimeError(f"failed to download {remote_path}")
        return result.returncode == 0
