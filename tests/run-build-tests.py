#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise target orchestration and launch/install gates with fake tool binaries."""

import json
import os
from pathlib import Path
import subprocess
import signal
import time
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
TARGETS = ["qemu_raspi3b", "qemu_raspi4b", "qemu_virt", "hw_raspi3b", "hw_raspi4b"]
MOCK = r"""#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
args = sys.argv[1:]
log = Path(os.environ['BUILD_TEST_LOG'])
with log.open('a') as f: f.write(json.dumps({'tool':Path(sys.argv[0]).name,'args':args})+'\n')
if Path(sys.argv[0]).name != 'cmake':
    sys.exit(43 if os.environ.get('BUILD_TEST_FAIL_TOOL') == Path(sys.argv[0]).name else 0)
if '--build' not in args:
    directory = Path(args[args.index('-B')+1])
    source = args[args.index('-S')+1]
    variables = dict(a[2:].split('=',1) for a in args if a.startswith('-D') and '=' in a)
    data = {'producer':source.rstrip('/').endswith('/cmake/userfs'),'variables':variables}
    directory.mkdir(parents=True,exist_ok=True)
    (directory/'mock.json').write_text(json.dumps(data))
else:
    directory = Path(args[args.index('--build')+1])
    data = json.loads((directory/'mock.json').read_text())
    variables = data['variables']
    target = variables.get('KORAOS_TARGET','userfs')
    if os.environ.get('BUILD_TEST_FAIL') == target: sys.exit(42)
    operation = args[args.index('--target')+1] if '--target' in args else ''
    if operation.startswith('install_'):
        destination = Path(variables['BOOTMNT'])
        destination.mkdir(parents=True,exist_ok=True)
        name = 'kernel8-rpi3.img' if target=='hw_raspi3b' else 'kernel8-rpi4.img'
        (destination/name).write_bytes((directory/'kernel.img').read_bytes())
    elif data['producer']:
        (directory/'user').mkdir(exist_ok=True)
        (directory/'user/hello.elf').write_bytes(b'ELF fixture')
        (directory/'koraos.img').write_bytes(b'FAT fixture')
    else:
        (directory/'kernel.img').write_bytes(target.encode())
        (directory/'kernel.elf').write_bytes(target.encode())
        (directory/'kernel.map').write_text(target)
        (directory/'compile_commands.json').write_text(json.dumps([{'file':'kernel.c','command':target}]))
"""


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="kora build tests ")
        self.base = Path(self.temp.name).resolve()
        self.bin = self.base / "tools"
        self.bin.mkdir()
        for tool in ("cmake", "qemu-system-aarch64", "mformat", "mcopy"):
            path = self.bin / tool
            path.write_text(MOCK)
            path.chmod(0o755)
        self.build = self.base / "output with spaces"
        self.log = self.base / "calls.jsonl"
        self.env = os.environ.copy()
        for key in (
            "BUILD_DIR",
            "BUILD_TYPE",
            "RPI_VERSION",
            "BOOTMNT",
            "KORA_QEMU_TARGET",
        ):
            self.env.pop(key, None)
        self.env.update(
            PATH=str(self.bin) + os.pathsep + self.env["PATH"],
            BUILD_TEST_LOG=str(self.log),
        )

    def tearDown(self):
        self.temp.cleanup()

    def invoke(self, *args, ok=True, script="build.sh", env=None):
        result = subprocess.run(
            [str(ROOT / script), "--build-dir", str(self.build), *args],
            cwd=self.base,
            env=env or self.env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        if ok:
            self.assertEqual(result.returncode, 0, result.stdout)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def calls(self):
        return (
            [json.loads(line) for line in self.log.read_text().splitlines()]
            if self.log.exists()
            else []
        )

    def configs(self):
        return [c for c in self.calls() if c["tool"] == "cmake" and "-B" in c["args"]]

    def selections(self):
        return [
            next(
                a.split("=", 1)[1]
                for a in c["args"]
                if a.startswith("-DKORAOS_TARGET=")
            )
            for c in self.configs()
            if any(a.startswith("-DKORAOS_TARGET=") for a in c["args"])
        ]

    def prepared(self):
        directory = self.base / "prepared userfs"
        directory.mkdir()
        (directory / "koraos.img").write_bytes(b"prepared FAT fixture")
        return directory

    def test_all_and_shared_producer(self):
        self.invoke("--target", "all")
        self.assertCountEqual(self.selections(), TARGETS)
        producers = [
            c
            for c in self.configs()
            if not any(a.startswith("-DKORAOS_TARGET=") for a in c["args"])
        ]
        self.assertEqual(len(producers), 1)
        shared = str(self.build / "userfs/aarch64")
        for c in self.configs()[1:]:
            self.assertIn("-DKORAOS_USERFS_DIR=" + shared, c["args"])
        self.assertTrue((self.build / "userfs/aarch64/koraos.img").is_file())
        for target in TARGETS:
            self.assertTrue((self.build / "debug" / target / "kernel.img").is_file())
        self.assertEqual(
            json.loads((self.build / "compile_commands.json").read_text())[0][
                "command"
            ],
            self.selections()[0],
        )
        self.assertFalse(
            any(
                "--target" in c["args"] and "install_hw" in c["args"]
                for c in self.calls()
            )
        )

    def test_default_is_all(self):
        self.invoke()
        self.assertCountEqual(self.selections(), TARGETS)

    def test_repeat_selection_and_release_isolation(self):
        self.invoke(
            "--target",
            "hw_raspi4b",
            "--target",
            "qemu_virt",
            "--target",
            "hw_raspi4b",
            "--release",
        )
        self.assertEqual(self.selections(), ["hw_raspi4b", "qemu_virt"])
        self.assertTrue((self.build / "release/hw_raspi4b/kernel.img").exists())
        self.assertFalse((self.build / "debug/hw_raspi4b").exists())
        self.log.unlink()
        self.invoke("--target", "qemu_virt")
        self.assertTrue((self.build / "debug/qemu_virt/kernel.img").exists())
        self.assertTrue((self.build / "release/qemu_virt/kernel.img").exists())

    def test_prepared_userfs_is_not_rebuilt(self):
        directory = self.prepared()
        self.invoke("--target", "qemu_virt", "--userfs-dir", str(directory))
        self.assertEqual(len(self.configs()), 1)
        self.assertIn(
            "-DKORAOS_USERFS_DIR=" + str(directory), self.configs()[0]["args"]
        )
        self.assertEqual(
            (directory / "koraos.img").read_bytes(), b"prepared FAT fixture"
        )

    def test_producer_only(self):
        self.invoke("--userfs-only")
        self.assertEqual(self.selections(), [])
        self.assertTrue((self.build / "userfs/aarch64/koraos.img").exists())

    def test_install_after_every_build_and_only_hardware(self):
        destination = self.base / "boot volume"
        self.invoke(
            "--target",
            "qemu_virt",
            "--target",
            "hw_raspi3b",
            "--target",
            "hw_raspi4b",
            "--release",
            "--install-to",
            str(destination),
        )
        calls = self.calls()
        installs = [i for i, c in enumerate(calls) if "install_hw" in c["args"]]
        self.assertEqual(len(installs), 2)
        kernel_builds = [
            i
            for i, c in enumerate(calls)
            if "--build" in c["args"] and "install_hw" not in c["args"]
        ]
        self.assertLess(max(kernel_builds), min(installs))
        self.assertEqual(
            sorted(p.name for p in destination.iterdir()),
            ["kernel8-rpi3.img", "kernel8-rpi4.img"],
        )

    def test_failed_kernel_does_not_install(self):
        env = self.env.copy()
        env["BUILD_TEST_FAIL"] = "hw_raspi4b"
        destination = self.base / "boot"
        self.invoke(
            "--target",
            "hw_raspi3b",
            "--target",
            "hw_raspi4b",
            "--install-to",
            str(destination),
            ok=False,
            env=env,
        )
        self.assertFalse(destination.exists())
        self.assertFalse(any("install_hw" in c["args"] for c in self.calls()))

    def test_failed_producer_does_not_build_kernels(self):
        env = self.env.copy()
        env["BUILD_TEST_FAIL"] = "userfs"
        self.invoke("--target", "qemu_virt", ok=False, env=env)
        self.assertEqual(self.selections(), [])

    def test_invalid_input_precedes_clean(self):
        self.build.mkdir()
        marker = self.build / "keep"
        marker.write_text("unrelated")
        self.invoke("--target", "bogus", "--clean", ok=False)
        self.assertTrue(marker.exists())
        self.assertEqual(self.calls(), [])
        self.invoke(
            "--target", "qemu_virt", "--install-to", str(self.base / "boot"), ok=False
        )
        self.assertEqual(self.calls(), [])

    def test_missing_prepared_image(self):
        self.invoke(
            "--target",
            "qemu_virt",
            "--userfs-dir",
            str(self.base / "missing"),
            ok=False,
        )
        self.assertEqual(self.calls(), [])

    def test_clean_preserves_unselected_and_external(self):
        directory = self.prepared()
        sentinel = self.build / "release/hw_raspi4b/keep"
        sentinel.parent.mkdir(parents=True)
        sentinel.write_text("keep")
        self.invoke("--target", "qemu_virt", "--clean", "--userfs-dir", str(directory))
        self.assertTrue(sentinel.exists())
        self.assertEqual(
            (directory / "koraos.img").read_bytes(), b"prepared FAT fixture"
        )

    def test_legacy_aliases(self):
        self.invoke("--virt")
        self.assertEqual(self.selections(), ["qemu_virt"])
        self.log.unlink()
        env = self.env.copy()
        env["RPI_VERSION"] = "3"
        self.invoke("--hw", env=env)
        self.assertEqual(self.selections(), ["hw_raspi3b"])

    def test_launcher_target_configuration_and_forwarding(self):
        image = self.build / "release/qemu_virt/kernel.img"
        image.parent.mkdir(parents=True)
        image.write_bytes(b"image")
        self.invoke(
            "--target", "qemu_virt", "--release", "-s", "-S", script="run-qemu.sh"
        )
        call = self.calls()[-1]
        self.assertEqual(call["tool"], "qemu-system-aarch64")
        self.assertEqual(call["args"][call["args"].index("-kernel") + 1], str(image))
        self.assertEqual(call["args"][-2:], ["-s", "-S"])
        self.log.unlink()
        self.invoke("--target", "hw_raspi3b", script="run-qemu.sh", ok=False)
        self.assertEqual(self.calls(), [])

    def test_sd_directory_output_rejected(self):
        for target in ("hw_raspi3b", "hw_raspi4b"):
            image = self.build / "debug" / target / "kernel.img"
            image.parent.mkdir(parents=True, exist_ok=True)
            image.write_bytes(b"kernel")
        destination = self.base / "directory output"
        destination.mkdir()
        result = self.invoke(
            "--target",
            "all",
            "--output",
            str(destination),
            script="create-sd-image.sh",
            ok=False,
        )
        self.assertIn("output", result.stdout.lower())
        self.assertEqual(list(destination.iterdir()), [])
        self.assertEqual(self.calls(), [])

    def test_sd_population_failure_preserves_existing_image(self):
        for target in ("hw_raspi3b", "hw_raspi4b"):
            image = self.build / "debug" / target / "kernel.img"
            image.parent.mkdir(parents=True, exist_ok=True)
            image.write_bytes(b"kernel")
        destination = self.base / "boot.img"
        destination.write_bytes(b"valid prior image")
        env = self.env.copy()
        env["BUILD_TEST_FAIL_TOOL"] = "mcopy"
        self.invoke(
            "--target",
            "all",
            "--output",
            str(destination),
            script="create-sd-image.sh",
            ok=False,
            env=env,
        )
        self.assertEqual(destination.read_bytes(), b"valid prior image")
        self.assertFalse(list(self.base.glob(".koraos-boot-image.*")))


class LockTests(unittest.TestCase):
    def test_terminated_owner_waits_for_child(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            pidfile = base / "pid"
            lock = base / "lock"
            code = (
                "import os,time; from pathlib import Path; Path(%r).write_text(str(os.getpid())); time.sleep(30)"
                % str(pidfile)
            )
            helper = ROOT / "scripts/with-userfs-lock.py"
            owner = subprocess.Popen(
                ["python3", str(helper), str(lock), "python3", "-c", code],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            child = None
            try:
                deadline = time.monotonic() + 5
                while not pidfile.exists() and time.monotonic() < deadline:
                    time.sleep(0.02)
                self.assertTrue(pidfile.exists(), "lock child did not start")
                child = int(pidfile.read_text())
                owner.send_signal(signal.SIGTERM)
                owner.communicate(timeout=5)
                with self.assertRaises(ProcessLookupError):
                    os.kill(child, 0)
                second = subprocess.run(
                    ["python3", str(helper), str(lock), "python3", "-c", "pass"],
                    capture_output=True,
                    timeout=5,
                )
                self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
            finally:
                if owner.poll() is None:
                    owner.kill()
                    owner.communicate()
                if child:
                    try:
                        os.kill(child, signal.SIGKILL)
                    except ProcessLookupError:
                        pass

    def test_killed_wrapper_retains_lock_until_child_exits(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            pidfile = base / "pid"
            lock = base / "lock"
            code = (
                "import os,time; from pathlib import Path; Path(%r).write_text(str(os.getpid())); time.sleep(2)"
                % str(pidfile)
            )
            helper = ROOT / "scripts/with-userfs-lock.py"
            owner = subprocess.Popen(
                ["python3", str(helper), str(lock), "python3", "-c", code],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            child = None
            try:
                deadline = time.monotonic() + 5
                while not pidfile.exists() and time.monotonic() < deadline:
                    time.sleep(0.02)
                self.assertTrue(pidfile.exists())
                child = int(pidfile.read_text())
                owner.kill()
                owner.wait(timeout=5)
                env = dict(os.environ, KORAOS_USERFS_LOCK_TIMEOUT="0.2")
                second = subprocess.run(
                    ["python3", str(helper), str(lock), "python3", "-c", "pass"],
                    env=env,
                    capture_output=True,
                    timeout=5,
                )
                self.assertNotEqual(
                    second.returncode, 0, "lock released with live orphan child"
                )
            finally:
                if owner.poll() is None:
                    owner.kill()
                    owner.wait()
                if child:
                    try:
                        os.kill(child, signal.SIGTERM)
                    except ProcessLookupError:
                        pass


if __name__ == "__main__":
    unittest.main(verbosity=2)
