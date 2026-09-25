import argparse
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from bench.board import Local  # noqa: E402
from bench.server import ServerManager  # noqa: E402
from bench.x86_dynamic import X86DynamicStrategy  # noqa: E402
from bench.x86_scx_llm_pin import X86ScxLlmPinStrategy  # noqa: E402

RUNTIME_BIN = "/opt/x86/bin"
CONTROLLER = "/opt/x86/dt_controller"
SCX_BIN = "/opt/x86/x86_scx_llm"
MODEL = "/models/example.gguf"


class FakeResult:
    def __init__(self, returncode=0, stdout=""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = ""


class FakeRemote:
    """Minimal transport stub that pretends the controller is up."""

    def __init__(self):
        self.commands = []

    def run(self, command, check=True, timeout=None):
        self.commands.append(command)
        if command.startswith("test -S"):
            return FakeResult(0)
        if "_scx.log" in command and command.startswith("cat"):
            return FakeResult(0, "scx_llm_pin attached successfully\n")
        return FakeResult(0, "")

    def upload(self, local_path, remote_path):
        pass

    def download(self, remote_path, local_path, optional=False):
        return True


def make_args(**overrides):
    args = argparse.Namespace(
        local=True,
        llm_path=None,
        runtime_bin_dir="",
        context=4096,
        x86_runtime_bin=RUNTIME_BIN,
        x86_model=MODEL,
        x86_dynamic_controller=CONTROLLER,
        x86_scx_llm_bin=SCX_BIN,
        x86_threads=4,
        x86_control_dir="/sys/fs/bpf/scx_rvv_dirty_lease",
        user_hint="on",
        scx_vector_slice_ms=100,
        scx_vector_local_window_ms=500,
        scx_dirty_threshold=2,
        scx_phase_hint_timeout_ms=300000,
        scx_prefill_starvation_ms=1000,
        scx_phase_wakeup_preempt="on",
    )
    for key, value in overrides.items():
        setattr(args, key, value)
    return args


class LocalTransportTest(unittest.TestCase):
    def test_run_returns_result_and_raises(self):
        local = Local()
        result = local.run("printf hello")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "hello")
        with self.assertRaises(RuntimeError):
            local.run("exit 3")

    def test_upload_download_roundtrip(self):
        local = Local()
        with tempfile.TemporaryDirectory() as root:
            source = Path(root) / "source.txt"
            source.write_text("payload")
            remote = Path(root) / "nested" / "remote.txt"
            local.upload(source, remote)
            self.assertEqual(remote.read_text(), "payload")
            back = Path(root) / "back.txt"
            self.assertTrue(local.download(remote, back))
            self.assertEqual(back.read_text(), "payload")
            self.assertFalse(local.download(Path(root) / "missing", back, optional=True))


class DirectServerArgvTest(unittest.TestCase):
    def test_direct_exec_skips_loader(self):
        manager = ServerManager(Local(), make_args(), "/tmp/bench-x86")
        argv = manager._argv(
            9191, MODEL, 4, 4, 1,
            {"direct": True, "runtime_bin": RUNTIME_BIN, "library_path": RUNTIME_BIN},
        )
        self.assertEqual(argv[0], f"{RUNTIME_BIN}/llama-server")
        self.assertNotIn("--library-path", argv)
        self.assertEqual(argv[argv.index("-t") + 1], "4")

    def test_loader_path_is_kept_for_riscv(self):
        args = make_args(
            local=False,
            llm_path="/mnt/llm/llama",
            runtime_bin_dir="/mnt/llm/llama/bin",
        )
        manager = ServerManager(Local(), args, "/tmp/bench-riscv")
        argv = manager._argv(9091, MODEL, 8, 8, 1, {})
        self.assertTrue(argv[0].endswith("ld-linux-riscv64-lp64d.so.1"))
        self.assertIn("--library-path", argv)


class X86DynamicStrategyTest(unittest.TestCase):
    def test_overrides_and_controller_command(self):
        strategy = X86DynamicStrategy(make_args(), FakeRemote(), "/tmp/bench-x86")
        overrides = strategy.server_overrides()
        self.assertTrue(overrides["direct"])
        self.assertEqual(overrides["runtime_bin"], RUNTIME_BIN)
        self.assertEqual(overrides["extra_env"]["OMP_DYNAMIC"], "FALSE")
        self.assertEqual(
            overrides["extra_env"]["GGML_DYNAMIC_THREADS_SOCKET"],
            "/tmp/bench-x86/dynamic.sock",
        )
        with mock.patch("bench.x86_dynamic.start_background", return_value=4242) as start:
            strategy.start("run01")
        command = start.call_args.args[1]
        for option in (
            "--mode dynamic", "--scx", "--grant-lifetime persistent",
            "--allocation-policy prefill-first", "--decode-wait-ms 0",
            "--decode-floor-on-prefill", "--socket",
        ):
            self.assertIn(option, command)
        self.assertIn("/run01.events.csv", command)


class X86ScxLlmPinStrategyTest(unittest.TestCase):
    def test_overrides_and_scheduler_command(self):
        strategy = X86ScxLlmPinStrategy(make_args(), FakeRemote(), "/tmp/bench-x86")
        overrides = strategy.server_overrides()
        self.assertTrue(overrides["direct"])
        self.assertEqual(
            overrides["extra_env"]["GGML_SCX_LEASE_HINT_PATH"],
            "/sys/fs/bpf/scx_rvv_dirty_lease/workers",
        )
        pids = {"A": 100, "B": 101}
        specs = [
            {"model": "A", "threads": 4, "threads_batch": 4, "cpu_mask": None},
            {"model": "B", "threads": 4, "threads_batch": 4, "cpu_mask": None},
        ]
        with mock.patch(
            "bench.x86_scx_llm_pin.start_background", return_value=55
        ) as start:
            strategy.start("run01", pids, specs)
        command = start.call_args.args[1]
        self.assertIn(SCX_BIN, command)
        self.assertIn("--team 100:4:4:0xff", command)
        self.assertIn("--team 101:4:4:0xff", command)


if __name__ == "__main__":
    unittest.main()
