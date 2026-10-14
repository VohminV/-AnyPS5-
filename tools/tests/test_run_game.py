"""Tests for tools/run_game.convert_game_tree (no binaries executed)."""

import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_game


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


class ConvertGameTreeTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        self.game = self.root / "game"
        (self.game / "sce_module").mkdir(parents=True)
        (self.game / "input.elf").write_bytes(build_elf())
        (self.game / "sce_module" / "mod.prx").write_bytes(build_elf())
        (self.game / "sce_module" / "readme.txt").write_text("not ELF")
        self.out = self.root / "out"

    def test_clean_elf_tree_is_mirrored_without_conversion(self):
        main, note = run_game.convert_game_tree(
            "nonexistent-inspector", str(self.game), str(self.game / "input.elf"), self.out)
        self.assertIsNotNone(main)
        self.assertEqual(main, self.out / "input.elf")
        self.assertEqual((self.out / "input.elf").read_bytes(), build_elf())
        self.assertEqual((self.out / "sce_module" / "mod.prx").read_bytes(), build_elf())
        self.assertFalse((self.out / "sce_module" / "readme.txt").exists())
        self.assertIn("copied clean ELF", note)

    def test_self_without_converter_fails_cleanly(self):
        (self.game / "eboot.bin").write_bytes(bytes([0x4F, 0x15, 0x3D, 0x1D]) + bytes(0x100))
        main, note = run_game.convert_game_tree(
            "nonexistent-inspector", str(self.game), str(self.game / "eboot.bin"), self.out)
        self.assertIsNone(main)
        self.assertIn("FAILED", note)
        # The original is preserved; nothing usable was produced.
        self.assertTrue((self.game / "eboot.bin").is_file())
        self.assertFalse((self.out / "eboot.bin").is_file())

    def test_originals_are_never_modified(self):
        before = {p: p.read_bytes() for p in
                  (self.game / "input.elf", self.game / "sce_module" / "mod.prx")}
        run_game.convert_game_tree(
            "nonexistent-inspector", str(self.game), str(self.game / "input.elf"), self.out)
        for path, data in before.items():
            self.assertEqual(path.read_bytes(), data)


if __name__ == "__main__":
    unittest.main()
