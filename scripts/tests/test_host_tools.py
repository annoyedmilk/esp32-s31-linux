# SPDX-License-Identifier: MIT
"""Exercise host selection and relocatable scripts without an SDK install."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SCRIPTS = Path(__file__).resolve().parents[1]


class HostToolsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="host tools ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.project = self.root / "relocated checkout"
        self.project.mkdir()
        self.env = os.environ.copy()
        for name in ("PYTHON", "IDF_PATH", "IDF_TOOLS_PATH", "IDF_PYTHON_ENV_PATH",
                     "BR_OUTPUT", "BR_DL_DIR", "BOARD"):
            self.env.pop(name, None)
        self.env.update(HOME=str(self.root / "home"), GIT_CONFIG_GLOBAL=os.devnull,
                        GIT_CONFIG_NOSYSTEM="1")
        self.sdk = self.project / "external/esp-idf"
        version = self.sdk / "tools/cmake/version.cmake"
        version.parent.mkdir(parents=True)
        version.write_text("set(IDF_VERSION_MAJOR 6)\nset(IDF_VERSION_MINOR 1)\n")
        self.tools = self.root / "custom tools"
        self.env["IDF_TOOLS_PATH"] = str(self.tools)
        self.copy_script("idf-python.sh")

    def copy_script(self, name):
        directory = self.project / "scripts"
        directory.mkdir(exist_ok=True)
        target = directory / name
        shutil.copy2(SCRIPTS / name, target)
        return target

    def executable(self, path, source="#!/bin/sh\nexit 0\n"):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(source)
        path.chmod(0o755)
        return path

    def interpreter(self, version):
        return self.executable(self.tools / f"python_env/idf{version}_py3.14_env/bin/python")

    def select_python(self):
        return subprocess.run(
            [str(self.project / "scripts/idf-python.sh")], env=self.env,
            cwd=self.root, capture_output=True, text=True, timeout=10,
        )

    def test_matches_sdk_version_in_custom_tools_directory(self):
        expected = self.interpreter("6.1")
        self.interpreter("6.2")
        result = self.select_python()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), str(expected))

    def test_explicit_python_wins_over_activated_environment(self):
        expected = self.executable(self.root / "selected interpreter/bin/python")
        self.env.update(PYTHON=str(expected), IDF_PYTHON_ENV_PATH="missing")
        result = self.select_python()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), str(expected))

    def test_activated_environment_does_not_require_default_layout(self):
        expected = self.executable(self.root / "active environment/bin/python")
        self.env["IDF_PYTHON_ENV_PATH"] = str(expected.parent.parent)
        result = self.select_python()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), str(expected))

    def test_missing_explicit_environment_does_not_silently_fall_back(self):
        self.interpreter("6.1")
        self.env["IDF_PYTHON_ENV_PATH"] = str(self.root / "missing")
        result = self.select_python()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")

    def test_other_sdk_version_is_not_selected(self):
        self.interpreter("6.2")
        self.assertNotEqual(self.select_python().returncode, 0)

    def git(self, *args):
        return subprocess.check_output(
            ["git", "-C", str(self.sdk), *args], env=self.env,
            stderr=subprocess.PIPE, text=True,
        ).strip()

    def initialize_sdk(self):
        self.git("init", "-q")
        for filename in ("rom.txt", "rcp.txt", "backport.txt"):
            (self.sdk / filename).write_text("original\n")
        self.git("add", ".")
        self.git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                 "commit", "-qm", "SDK base")
        bootloader = self.project / "bootloader"
        bootloader.mkdir()
        (bootloader / "esp-idf.version").write_text(self.git("rev-parse", "HEAD") + "\n")

    def test_patch_script_is_relocatable_and_idempotent(self):
        self.initialize_sdk()
        script = self.copy_script("patch-esp-idf.sh")
        patches = (
            ("external/esp-hosted-linux/esp/esp_driver/lib/rom.patch", "rom.txt"),
            ("external/esp-hosted-linux/esp/esp_driver/lib/idf-openthread-custom-rcp-v6.1.patch", "rcp.txt"),
            ("bootloader/idf-patches/0001-backport.patch", "backport.txt"),
        )
        for relative, filename in patches:
            target = self.project / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(
                f"diff --git a/{filename} b/{filename}\n"
                f"--- a/{filename}\n+++ b/{filename}\n"
                "@@ -1 +1 @@\n-original\n+patched\n"
            )
        for attempt in range(2):
            subprocess.run([str(script)], env=self.env, cwd=self.root,
                           capture_output=True, check=True, timeout=15)
            current = self.git("rev-parse", "HEAD")
            if attempt:
                self.assertEqual(current, previous)
            previous = current
        self.assertEqual(self.git("status", "--porcelain"), "")
        for _, filename in patches:
            self.assertEqual((self.sdk / filename).read_text(), "patched\n")

    def test_buildroot_arguments_preserve_custom_output_path(self):
        self.initialize_sdk()
        self.git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                 "commit", "--allow-empty", "-qm", "Patched SDK")
        script = self.copy_script("ci-build.sh")
        self.executable(self.project / "scripts/mkdefconfigs.sh")
        for relative in (
            "linux/example.c", "shared/example.h",
            "br2-external/board/esp32s31/linux.config",
            "br2-external/configs/esp32s31_defconfig", "br2-external/external.mk",
            "br2-external/linux/example.mk", "br2-external/package/esp-hosted-linux/example.mk",
            "external/esp-hosted-linux/host/example.c",
            "external/esp-hosted-linux/host/include/example.h",
        ):
            path = self.project / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture\n")
        (self.project / "bootloader/partitions-esp32s31.csv").write_text(
            "linux, data, 0x40, 0x210000, 0x400000\n"
            "rootfs, data, 0x40, 0x610000, 0x780000\n"
        )
        output = self.root / "custom output"
        calls = self.root / "make-calls.jsonl"
        self.env.update(BR_OUTPUT=str(output), MAKE_CALLS=str(calls))
        binary = self.root / "bin"
        self.env["PATH"] = str(binary) + os.pathsep + self.env["PATH"]
        make_fixture = """#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys

with open(os.environ['MAKE_CALLS'], 'a') as log:
    log.write(json.dumps(sys.argv[1:]) + '\\n')
for argument in sys.argv[1:]:
    if argument.startswith('O='):
        images = Path(argument[2:]) / 'images'
        images.mkdir(parents=True, exist_ok=True)
        for name in ('Image', 'linux.size', 'rootfs.squashfs'):
            (images / name).write_bytes(b'fixture')
"""
        self.executable(binary / "make", make_fixture)
        subprocess.run([str(script)], env=self.env, cwd=self.root,
                       capture_output=True, check=True, timeout=15)
        invocations = [json.loads(line) for line in calls.read_text().splitlines()]
        buildroot = [args for args in invocations if "external/buildroot" in args]
        self.assertGreaterEqual(len(buildroot), 3)
        for args in buildroot:
            self.assertIn("O=" + str(output), args)
            self.assertIn("BR2_EXTERNAL=" + str(self.project / "br2-external"), args)
        self.assertEqual((self.project / "build/esp32s31/Image").read_bytes(), b"fixture")


if __name__ == "__main__":
    unittest.main()
