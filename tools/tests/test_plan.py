"""Tests for tools/plan.py (synthetic game dirs, no Relinker run)."""

import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import plan
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


class PlanTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        (self.root / "sce_module").mkdir()
        (self.root / "input.elf").write_bytes(build_elf())
        (self.root / "sce_module" / "mod.prx").write_bytes(build_elf())
        self.relinker = self.root / "relinker-fake"
        self.relinker.write_bytes(b"fake")
        libs = self.root / "fakebuild" / "core" / "libs" / "libs"
        libs.mkdir(parents=True)
        (libs / "libSceTest.prx").write_bytes(build_elf())
        self.build_dir = str(self.root / "fakebuild")
        compiler = patch.object(preflight_mod, "check_compiler",
                                return_value=("g++", "/fake/g++", "g++ fake 15.2.0"))
        compiler.start()
        self.addCleanup(compiler.stop)
        vulkan = patch.object(preflight_mod, "check_vulkan",
                              return_value={"present": True, "ok": True, "detail": "fake",
                                            "summary": "fake loader+device",
                                            "layers": {"loader": {"status": "present"}}})
        vulkan.start()
        self.addCleanup(vulkan.stop)

    def names(self, built):
        return [stage["name"] for stage in built["stages"]]

    def test_prepare_plan_stages_and_target_argv(self):
        built, blocked = plan.build_plan(
            target="linux", game_dir=str(self.root),
            input_path=str(self.root / "input.elf"),
            output_path=str(self.root / "out.elf"),
            relinker_path=str(self.relinker), build_dir=self.build_dir)
        self.assertIsNone(blocked)
        self.assertEqual(self.names(built),
                         ["PREFLIGHT", "RELINK", "RUNTIME_START", "GAME_RUNNING"])
        relink = next(s for s in built["stages"] if s["name"] == "RELINK")
        self.assertNotIn("--windows", relink["command"])
        self.assertEqual(relink["timeout"], 300)
        self.assertTrue(relink["expected_artifacts"][0].endswith("out.elf"))
        for stage in built["stages"]:
            for key in ("goal", "inputs", "preconditions", "tool", "command",
                        "expected_artifacts", "success_criteria", "timeout",
                        "verify", "on_error"):
                self.assertIn(key, stage)

    def test_windows_target_selects_windows_flag_once(self):
        built, blocked = plan.build_plan(
            target="windows", game_dir=str(self.root),
            input_path=str(self.root / "input.elf"),
            output_path=str(self.root / "app.exe"),
            relinker_path=str(self.relinker), build_dir=self.build_dir)
        self.assertIsNone(blocked)
        relink = next(s for s in built["stages"] if s["name"] == "RELINK")
        self.assertEqual(relink["command"].count("--windows"), 1)

    def test_self_input_plans_conversion_before_relink(self):
        game = self.root / "selfgame"
        (game / "sce_module").mkdir(parents=True)
        (game / "eboot.bin").write_bytes(bytes([0x4F, 0x15, 0x3D, 0x1D]) + bytes(0x100))
        (game / "sce_module" / "mod.prx").write_bytes(build_elf())
        # No converter: the plan blocks here, never at Relinker.
        with patch.object(preflight_mod, "find_self_inspector", return_value=""), \
             patch.object(preflight_mod, "find_relinker", return_value=""):
            built, blocked = plan.build_plan(
                target="linux", game_dir=str(game), input_path=str(game / "eboot.bin"),
                relinker_path=str(self.relinker), build_dir=self.build_dir)
        self.assertIn("CONVERT_SELF", self.names(built))
        convert = next(s for s in built["stages"] if s["name"] == "CONVERT_SELF")
        # No converter on this machine: the plan blocks here, never at Relinker.
        self.assertIsNotNone(blocked)
        self.assertEqual(blocked["stage"], "CONVERT_SELF")
        self.assertEqual(convert["command"], [])

    def test_self_with_converter_plans_conversion_without_blocking(self):
        game = self.root / "selfgame2"
        (game / "sce_module").mkdir(parents=True)
        (game / "eboot.bin").write_bytes(bytes([0x4F, 0x15, 0x3D, 0x1D]) + bytes(0x100))
        (game / "sce_module" / "mod.prx").write_bytes(build_elf())
        inspector = self.root / "self_inspector-fake"
        inspector.write_bytes(b"x")
        with patch.object(preflight_mod, "find_self_inspector",
                          return_value=str(inspector)):
            built, blocked = plan.build_plan(
                target="linux", game_dir=str(game), input_path=str(game / "eboot.bin"),
                relinker_path=str(self.relinker), build_dir=self.build_dir)
        self.assertIsNone(blocked)
        convert = next(s for s in built["stages"] if s["name"] == "CONVERT_SELF")
        self.assertTrue(convert["command"])
        self.assertIn("--extract", convert["command"])
        relink = next(s for s in built["stages"] if s["name"] == "RELINK")
        self.assertTrue(any("pending CONVERT_SELF" in p for p in relink["preconditions"]))
        self.assertFalse(any(p.startswith("MISSING") for p in relink["preconditions"]))

    def test_runtime_gates_do_not_block_preparation(self):
        # No libs dir and real Vulkan state: RUNTIME_START is the first
        # blocked stage, but CONVERT_SELF/RELINK stay executable.
        game = self.root / "selfgame3"
        (game / "sce_module").mkdir(parents=True)
        (game / "eboot.bin").write_bytes(bytes([0x4F, 0x15, 0x3D, 0x1D]) + bytes(0x100))
        (game / "sce_module" / "mod.prx").write_bytes(build_elf())
        inspector = self.root / "self_inspector-fake2"
        inspector.write_bytes(b"x")
        with patch.object(preflight_mod, "find_self_inspector",
                          return_value=str(inspector)):
            built, blocked = plan.build_plan(
                target="linux", game_dir=str(game), input_path=str(game / "eboot.bin"),
                relinker_path=str(self.relinker))
        self.assertIsNotNone(blocked)
        self.assertEqual(blocked["stage"], "RUNTIME_START")
        convert = next(s for s in built["stages"] if s["name"] == "CONVERT_SELF")
        self.assertTrue(convert["command"])

    def test_blocked_plan_names_stage_and_next_action(self):
        with patch.object(preflight_mod, "find_relinker", return_value=""):
            built, blocked = plan.build_plan(
                target="linux", game_dir=str(self.root),
                input_path=str(self.root / "input.elf"),
                output_path=str(self.root / "out.elf"),
                relinker_path=str(self.root / "absent-relinker"), build_dir=self.build_dir)
        self.assertIsNotNone(blocked)
        self.assertIn(blocked["stage"], ("RELINK", "RUNTIME_START"))
        self.assertTrue(blocked["reason"])
        self.assertIn("evidence", blocked)

    def test_run_flag_adds_launch_stage(self):
        built, _ = plan.build_plan(
            target="linux", game_dir=str(self.root),
            input_path=str(self.root / "input.elf"),
            output_path=str(self.root / "out.elf"),
            relinker_path=str(self.relinker), want_run=True)
        launch = next(s for s in built["stages"] if s["name"] == "GAME_RUNNING")
        self.assertTrue(launch["command"])
        self.assertEqual(launch["timeout"], 30)
        built2, _ = plan.build_plan(
            target="linux", game_dir=str(self.root),
            input_path=str(self.root / "input.elf"),
            output_path=str(self.root / "out.elf"),
            relinker_path=str(self.relinker), want_run=False)
        skipped = next(s for s in built2["stages"] if s["name"] == "GAME_RUNNING")
        self.assertEqual(skipped["command"], [])


if __name__ == "__main__":
    unittest.main()
