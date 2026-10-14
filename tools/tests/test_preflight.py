"""Tests for tools/preflight.py (synthetic inputs, temp dirs, mocked hosts)."""

import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import preflight as preflight_mod


def build_elf(total=0x80):
    header = bytearray(total)
    header[0:4] = b"\x7fELF"
    header[4] = 2
    header[5] = 1
    header[6] = 1
    struct.pack_into("<H", header, 0x12, 62)
    struct.pack_into("<H", header, 0x34, 64)
    struct.pack_into("<H", header, 0x36, 56)
    return bytes(header)


class HostDetectionTests(unittest.TestCase):
    def test_os_is_not_derived_from_filenames(self):
        host = preflight_mod.detect_host(system="Windows", machine="AMD64", env={})
        self.assertEqual((host["host_os"], host["host_arch"]), ("Windows", "x86_64"))
        host = preflight_mod.detect_host(system="Linux", machine="x86_64",
                                         env={"BASH_VERSION": "5"})
        self.assertEqual((host["host_os"], host["shell"]), ("Linux", "bash"))
        host = preflight_mod.detect_host(system="Darwin", machine="arm64", env={})
        self.assertEqual((host["host_os"], host["host_arch"]), ("macOS", "arm64"))
        host = preflight_mod.detect_host(system="FreeBSD", machine="riscv64", env={})
        self.assertEqual(host["host_os"], "unsupported")

    def test_shell_is_honest_when_ambiguous(self):
        host = preflight_mod.detect_host(system="Windows", machine="AMD64",
                                         env={"PSModulePath": "x", "COMSPEC": "cmd"})
        self.assertNotEqual(host["shell"], "PowerShell")
        self.assertIn("ambiguous", host["shell"])


class TargetArgvTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        (self.root / "sce_module").mkdir()
        self.input = self.root / "input.elf"
        self.input.write_bytes(build_elf())
        (self.root / "sce_module" / "mod.prx").write_bytes(build_elf())
        self.relinker = self.root / "relinker-fake"
        self.relinker.write_bytes(b"fake")
        self.out = self.root / "app.elf"
        compiler = patch.object(preflight_mod, "check_compiler",
                                return_value=("g++", "/fake/g++", "g++ fake 15.2.0"))
        compiler.start()
        self.addCleanup(compiler.stop)

    def check(self, **kwargs):
        kwargs.setdefault("game_dir", str(self.root))
        kwargs.setdefault("relinker_path", str(self.relinker))
        return preflight_mod.preflight(**kwargs)

    def test_windows_target_adds_windows_flag_once(self):
        report, _ = self.check(target="windows", output_path=str(self.root / "app.exe"),
                             host_override={"system": "Windows", "machine": "AMD64", "env": {}})
        self.assertEqual(report["relink_argv"][:2], [str(self.relinker), "--windows"])
        self.assertEqual(report["relink_argv"].count("--windows"), 1)

    def test_linux_target_has_no_windows_flag(self):
        report, _ = self.check(target="linux", output_path=str(self.out),
                             host_override={"system": "Windows", "machine": "AMD64", "env": {}})
        self.assertNotIn("--windows", report["relink_argv"])

    def test_explicit_target_overrides_host(self):
        report, _ = self.check(target="windows", output_path=str(self.root / "app.exe"),
                             host_override={"system": "Linux", "machine": "x86_64", "env": {}})
        self.assertIn("--windows", report["relink_argv"])
        self.assertEqual(report["host_os"], "Linux")

    def test_linux_host_linux_target(self):
        report, status = self.check(target="linux", output_path=str(self.out),
                                  host_override={"system": "Linux", "machine": "x86_64",
                                                 "env": {"BASH_VERSION": "1"}})
        self.assertNotIn("--windows", report["relink_argv"])
        self.assertIn(status, ("READY", "READY_WITH_WARNINGS"))

    def test_macos_target_is_rejected(self):
        _, status = self.check(target="macos", output_path=str(self.out),
                             host_override={"system": "Linux", "machine": "x86_64", "env": {}})
        self.assertEqual(status, "BLOCKED_ENVIRONMENT")

    def test_cli_mismatch_blocks_without_trial_runs(self):
        with patch.object(preflight_mod, "relinker_supports_windows",
                          return_value=(False, "fake binary without the flag")):
            report, status = self.check(target="windows", output_path=str(self.root / "app.exe"))
        self.assertEqual(status, "BLOCKED_BUILD")
        self.assertEqual(report["relink_argv"], [])
        self.assertTrue(any("no --windows flag" in b["reason"] for b in report["blockers"]))

    def test_paths_with_spaces_stay_single_argv_elements(self):
        spaced = self.root / "dir with spaces"
        spaced.mkdir()
        game = spaced / "My Game"
        game.mkdir()
        (game / "sce_module").mkdir()
        (game / "input file.elf").write_bytes(build_elf())
        (game / "sce_module" / "mod.prx").write_bytes(build_elf())
        report, _ = preflight_mod.preflight(
            target="linux", game_dir=str(game),
            input_path=str(game / "input file.elf"),
            output_path=str(game / "out file.elf"),
            relinker_path=str(self.relinker))
        self.assertIn(str(game / "input file.elf"), report["relink_argv"])
        self.assertIn(str(game / "out file.elf"), report["relink_argv"])


class BlockerTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        compiler = patch.object(preflight_mod, "check_compiler",
                                return_value=("g++", "/fake/g++", "g++ fake 15.2.0"))
        compiler.start()
        self.addCleanup(compiler.stop)

    def test_missing_relinker_gives_host_specific_build_command(self):
        game = self.root / "game"
        (game / "sce_module").mkdir(parents=True)
        (game / "input.elf").write_bytes(build_elf())
        (game / "sce_module" / "mod.prx").write_bytes(build_elf())
        with patch.object(preflight_mod, "find_relinker", return_value=""):
            report, status = preflight_mod.preflight(
                target="linux", game_dir=str(game), input_path=str(game / "input.elf"),
                relinker_path=str(self.root / "absent"),
                host_override={"system": "Windows", "machine": "AMD64", "env": {}})
        self.assertEqual(status, "BLOCKED_BUILD")
        blocker = next(b for b in report["blockers"] if b["status"] == "BLOCKED_BUILD")
        self.assertIn("cmake --build build-relinker", blocker["next"])
        self.assertNotIn("chmod", blocker["next"])
        self.assertNotIn(".sh", blocker["next"])

    def test_self_never_reaches_relinker(self):
        game = self.root / "game"
        (game / "sce_module").mkdir(parents=True)
        (game / "eboot.bin").write_bytes(bytes([0x4F, 0x15, 0x3D, 0x1D]) + bytes(0x100))
        (game / "sce_module" / "mod.prx").write_bytes(build_elf())
        with patch.object(preflight_mod, "find_self_inspector", return_value=""):
            report, status = preflight_mod.preflight(
                target="linux", game_dir=str(game), input_path=str(game / "eboot.bin"),
                relinker_path=str(self.root / "absent"))
        self.assertEqual(status, "BLOCKED_INPUT_FORMAT")
        self.assertEqual(report["relink_argv"], [])
        self.assertEqual(report["input_inspection"]["format"], "SELF")

    def test_missing_and_conflicting_modules_block(self):
        empty = self.root / "empty"
        empty.mkdir()
        (empty / "input.elf").write_bytes(build_elf())
        report, status = preflight_mod.preflight(
            target="linux", game_dir=str(empty),
            input_path=str(empty / "input.elf"))
        self.assertEqual(status, "BLOCKED_INPUT_FORMAT")
        both = self.root / "both"
        (both / "sce_module").mkdir(parents=True)
        (both / "sce_modules").mkdir(parents=True)
        (both / "input.elf").write_bytes(build_elf())
        report, _ = preflight_mod.preflight(
            target="linux", game_dir=str(both), input_path=str(both / "input.elf"))
        self.assertTrue(any("Both sce_module" in b["reason"] for b in report["blockers"]))

    def test_input_output_collision_blocks(self):
        game = self.root / "game"
        (game / "sce_module").mkdir(parents=True)
        (game / "input.elf").write_bytes(build_elf())
        (game / "sce_module" / "mod.prx").write_bytes(build_elf())
        report, status = preflight_mod.preflight(
            target="linux", game_dir=str(game), input_path=str(game / "input.elf"),
            output_path=str(game / "input.elf"),
            relinker_path=str(game / "input.elf"))
        self.assertEqual(status, "BLOCKED_INPUT_FORMAT")
        self.assertTrue(any("overwrite the input" in b["reason"] for b in report["blockers"]))

    def test_originals_are_preserved(self):
        game = self.root / "game"
        (game / "sce_module").mkdir(parents=True)
        blob = build_elf()
        (game / "input.elf").write_bytes(blob)
        (game / "sce_module" / "mod.prx").write_bytes(blob)
        before = {(p, Path(p).read_bytes()) for p in
                  (game / "input.elf", game / "sce_module" / "mod.prx")}
        preflight_mod.preflight(target="linux", game_dir=str(game),
                                input_path=str(game / "input.elf"))
        for path, data in before:
            self.assertEqual(Path(path).read_bytes(), data)


class RepeatGuardTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.guard = Path(self._tmp.name) / "guard.json"
        compiler = patch.object(preflight_mod, "check_compiler",
                                return_value=("g++", "/fake/g++", "g++ fake 15.2.0"))
        compiler.start()
        self.addCleanup(compiler.stop)

    def test_identical_command_is_recognized(self):
        argv = ["relinker", "in.elf", "out.elf"]
        self.assertFalse(preflight_mod.is_repeat_command(argv, self.guard))
        preflight_mod.record_failure(argv, "test", self.guard)
        self.assertTrue(preflight_mod.is_repeat_command(argv, self.guard))
        self.assertFalse(preflight_mod.is_repeat_command(
            ["relinker", "--windows", "in.elf", "out.elf"], self.guard))
        data = json.loads(self.guard.read_text())
        self.assertEqual(data["argv"], argv)

    def test_preflight_warns_on_repeat(self):
        root = Path(self._tmp.name) / "game"
        (root / "sce_module").mkdir(parents=True)
        (root / "input.elf").write_bytes(build_elf())
        (root / "sce_module" / "mod.prx").write_bytes(build_elf())
        fake = Path(self._tmp.name) / "relinker"
        fake.write_bytes(b"x")
        with patch.object(preflight_mod, "GUARD_FILE", self.guard):
            first, _ = preflight_mod.preflight(
                target="linux", game_dir=str(root), input_path=str(root / "input.elf"),
                output_path=str(root / "out.elf"), relinker_path=str(fake))
            preflight_mod.record_failure(first["relink_argv"], "test", self.guard)
            second, _ = preflight_mod.preflight(
                target="linux", game_dir=str(root), input_path=str(root / "input.elf"),
                output_path=str(root / "out.elf"), relinker_path=str(fake))
        self.assertTrue(any("already failed" in w for w in second["warnings"]))


if __name__ == "__main__":
    unittest.main()
