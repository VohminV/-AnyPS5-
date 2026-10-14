"""Tests for tools/inspect_executable.py (synthetic headers only, no game dumps)."""

import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import inspect_executable as inspect_mod


def build_elf(*, machine=62, elf_class=2, data=1, version=1,
              ehsize=64, phentsize=56, phnum=0, phoff=0, magic=b"\x7fELF",
              total=0x80):
    header = bytearray(total)
    header[0:4] = magic
    header[4] = elf_class
    header[5] = data
    header[6] = version
    struct.pack_into("<H", header, 0x10, 2)  # e_type
    struct.pack_into("<H", header, 0x12, machine)
    struct.pack_into("<Q", header, 0x20, phoff)
    struct.pack_into("<H", header, 0x34, ehsize)
    struct.pack_into("<H", header, 0x36, phentsize)
    struct.pack_into("<H", header, 0x38, phnum)
    return bytes(header)


class InspectFileTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)

    def write(self, name, data):
        path = self.root / name
        path.write_bytes(data)
        return path

    def test_valid_elf_header_is_relinkable(self):
        info = inspect_mod.inspect_file(self.write("input.elf", build_elf()))
        self.assertEqual(info["format"], "ELF")
        self.assertTrue(info["ok"] and info["relinkable"])
        self.assertEqual(info["head_hex"][:11], "7f 45 4c 46")
        self.assertEqual(info["machine"], 62)

    def test_bin_extension_does_not_decide_format(self):
        info = inspect_mod.inspect_file(self.write("eboot.bin", build_elf()))
        self.assertEqual(info["format"], "ELF")
        self.assertTrue(info["relinkable"])
        other = inspect_mod.inspect_file(self.write("other.bin", b"\x00" * 0x80))
        self.assertNotEqual(other["format"], "ELF")
        self.assertFalse(other["relinkable"])

    def test_name_does_not_decide_format(self):
        info = inspect_mod.inspect_file(self.write("totally_elf.elf", b"\x00" * 0x80))
        self.assertNotEqual(info["format"], "ELF")

    def test_self_magics_are_rejected(self):
        for name, magic in (("eboot.self", bytes([0x4F, 0x15, 0x3D, 0x1D])),
                            ("eboot.kernel", bytes([0x54, 0x14, 0xF5, 0xEE]))):
            with self.subTest(name=name):
                info = inspect_mod.inspect_file(self.write(name, magic + bytes(0x100)))
                self.assertEqual(info["format"], "SELF")
                self.assertFalse(info["relinkable"])
                self.assertIn("SELF container", info["reason"])

    def test_self_is_not_converted_by_rename_or_copy(self):
        raw = bytes([0x4F, 0x15, 0x3D, 0x1D]) + bytes(0x100)
        first = self.write("a.self", raw)
        renamed = self.root / "input.elf"
        renamed.write_bytes(first.read_bytes())
        self.assertEqual(inspect_mod.inspect_file(renamed)["format"], "SELF")

    def test_pkg_and_unknown_formats(self):
        pkg = inspect_mod.inspect_file(self.write("x.pkg", b"\x7fCNT" + bytes(0x100)))
        self.assertEqual(pkg["format"], "PKG")
        self.assertIn("7f 43 4e 54", pkg["reason"])
        unknown = inspect_mod.inspect_file(self.write("x.elf", b"\x01\x02\x03\x04" + bytes(0x100)))
        self.assertEqual(unknown["format"], "UNKNOWN")
        self.assertIn("Invalid ELF magic number", unknown["reason"])

    def test_truncated_and_too_small(self):
        truncated = inspect_mod.inspect_file(self.write("short.elf", b"\x7fELF" + bytes(10)))
        self.assertEqual(truncated["format"], "TRUNCATED")
        self.assertIn("File too small for ELF header", truncated["reason"])
        tiny = inspect_mod.inspect_file(self.write("tiny", b"\x7fE"))
        self.assertEqual(tiny["format"], "TOO_SMALL")
        self.assertFalse(inspect_mod.inspect_file(self.root / "absent.elf")["relinkable"])

    def test_invalid_header_fields_rejected(self):
        for kwargs, needle in (
                ({"elf_class": 1}, "not 64-bit"),
                ({"data": 2}, "little-endian"),
                ({"machine": 40}, "unexpected machine"),
                ({"ehsize": 52}, "ehsize"),
                ({"phentsize": 32, "phnum": 2}, "phentsize")):
            with self.subTest(kwargs=kwargs):
                info = inspect_mod.inspect_file(self.write("bad.elf", build_elf(**kwargs)))
                self.assertEqual(info["format"], "ELF")
                self.assertFalse(info["ok"] or info["relinkable"])
                self.assertIn(needle, info["reason"])

    def test_ph_table_out_of_bounds(self):
        info = inspect_mod.inspect_file(
            self.write("ph.elf", build_elf(phnum=4, phoff=0x1000)))
        self.assertFalse(info["ok"])
        self.assertIn("program header table", info["reason"])

    def test_output_verification(self):
        elf = self.write("app.elf", build_elf())
        self.assertTrue(inspect_mod.inspect_output(elf, "linux")["ok"])
        self.assertFalse(inspect_mod.inspect_output(elf, "windows")["ok"])
        pe = self.write("app.exe", b"MZ" + bytes(0x100))
        self.assertTrue(inspect_mod.inspect_output(pe, "windows")["ok"])
        self.assertFalse(inspect_mod.inspect_output(pe, "linux")["ok"])

    def test_inspection_does_not_modify_originals(self):
        data = build_elf()
        path = self.write("eboot.bin", data)
        before = inspect_mod.sha256_of(path)
        inspect_mod.inspect_file(path)
        inspect_mod.inspect_game_dir(self.root)
        self.assertEqual(inspect_mod.sha256_of(path), before)
        self.assertEqual(path.read_bytes(), data)


class TableTests(unittest.TestCase):
    """Program/section header tables, bounds and structural honesty."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)

    def write(self, name, data):
        path = self.root / name
        path.write_bytes(data)
        return path

    def elf_with_tables(self, entries, shnum=0, shoff=0, total=0x400):
        header = bytearray(build_elf(total=total))
        phoff = 64
        struct.pack_into("<Q", header, 0x20, phoff)
        struct.pack_into("<H", header, 0x38, len(entries))
        struct.pack_into("<Q", header, 0x28, shoff)
        struct.pack_into("<H", header, 0x3A, 64)
        struct.pack_into("<H", header, 0x3C, shnum)
        for ptype, flags, offset, filesz in entries:
            header[phoff:phoff + 56] = struct.pack(
                "<IIQQQQQQ", ptype, flags, offset, 0x400000 + offset,
                offset, filesz, filesz, 0x1000)
            phoff += 56
        return bytes(header)

    def test_valid_tables_report_ranges_and_dynamic(self):
        data = bytearray(self.elf_with_tables([(1, 5, 0x200, 0x100), (2, 6, 0x300, 16)]))
        data[0x300:0x310] = struct.pack("<qQ", 0, 0)  # DT_NULL terminator
        info = inspect_mod.inspect_file(self.write("good.elf", bytes(data)))
        self.assertTrue(info["ok"] and info["relinkable"])
        self.assertTrue(info["has_dynamic"])
        self.assertEqual(len(info["program_headers"]), 2)
        load = info["program_headers"][0]
        self.assertEqual((load["type"], load["offset"], load["filesz"]), ("LOAD", 0x200, 0x100))
        self.assertTrue(load["in_bounds"])
        self.assertEqual(info["supported_ops"], ["relink"])

    def test_missing_dynamic_with_segments_is_rejected(self):
        info = inspect_mod.inspect_file(
            self.write("nodyn.elf", self.elf_with_tables([(1, 5, 0x200, 0x100)])))
        self.assertFalse(info["ok"] or info["relinkable"])
        self.assertIn("PT_DYNAMIC", info["reason"])

    def test_out_of_bounds_segment_is_rejected(self):
        info = inspect_mod.inspect_file(
            self.write("oob.elf", self.elf_with_tables([(1, 5, 0x2000, 0x100)])))
        self.assertFalse(info["ok"])
        self.assertIn("out of bounds", info["reason"])

    def test_excessive_table_counts_are_rejected(self):
        total = 64 + 65 * 56 + 16
        header = bytearray(build_elf(total=total))
        struct.pack_into("<Q", header, 0x20, 64)
        struct.pack_into("<H", header, 0x38, 65)
        info = inspect_mod.inspect_file(self.write("many.elf", bytes(header)))
        self.assertFalse(info["ok"])
        self.assertIn("sanity limit", info["reason"])

    def test_dangling_section_table_warns_without_blocking(self):
        # Relinker never reads section headers (no callers of
        # ElfReader::ReadSectionHeaders), so a dangling table as produced by
        # SELF extraction must warn, not block.
        data = bytearray(self.elf_with_tables([(1, 5, 0x200, 0x80), (2, 6, 0x300, 16)]))
        data[0x300:0x310] = struct.pack("<qQ", 0, 0)
        # Section header claiming a range past EOF.
        shoff = 0x120
        struct.pack_into("<Q", data, 0x28, shoff)
        struct.pack_into("<H", data, 0x3C, 1)
        data[shoff:shoff + 64] = struct.pack("<IIQQQQIIQQ", 0, 1, 0, 0, 0x5000, 0x100, 0, 0, 1, 0)
        info = inspect_mod.inspect_file(self.write("sh.elf", bytes(data)))
        self.assertTrue(info["ok"] and info["relinkable"])
        self.assertTrue(any("section header 0" in w for w in info["section_warnings"]))


class GameDirTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)

    def module(self, directory, name="mod.prx"):
        (self.root / directory).mkdir(parents=True, exist_ok=True)
        path = self.root / directory / name
        path.write_bytes(build_elf())
        return path

    def test_prx_coexists_with_sce_module(self):
        self.module("sce_module")
        self.module("prx")
        report = inspect_mod.inspect_game_dir(self.root)
        self.assertEqual(report["error"], "")
        self.assertEqual(report["elf_modules"], 2)

    def test_both_sce_dirs_conflict(self):
        self.module("sce_module")
        self.module("sce_modules")
        report = inspect_mod.inspect_game_dir(self.root)
        self.assertIn("Both sce_module and sce_modules", report["error"])

    def test_all_dirs_absent_is_error(self):
        report = inspect_mod.inspect_game_dir(self.root)
        self.assertIn("was not found", report["error"])

    def test_extra_module_dir_rules(self):
        self.module("sce_module")
        extra = self.root / "Media" / "Plugins"
        extra.mkdir(parents=True)
        (extra / "plugin.prx").write_bytes(build_elf())
        report = inspect_mod.inspect_game_dir(self.root, ["Media/Plugins"])
        self.assertEqual(report["error"], "")
        for bad in ("../outside", "Media/Missing", "sce_module"):
            with self.subTest(bad=bad):
                report = inspect_mod.inspect_game_dir(self.root, [bad])
                self.assertTrue(report["error"], bad)


if __name__ == "__main__":
    unittest.main()
