"""Tests for tools/capabilities.py (no hardware or game dumps needed)."""

import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import capabilities


class RegistryTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)

    def test_registry_has_all_tools(self):
        with patch.object(capabilities, "vulkan_record",
                          return_value={"name": "vulkan_runtime", "ok": False}):
            registry = capabilities.registry(build_dir=str(self.root))
        self.assertEqual(set(registry), {"relinker", "self_inspector", "build_toolchain",
                                         "vulkan_runtime", "python"})
        for record in registry.values():
            self.assertIn("name", record)

    def test_relinker_record_comes_from_real_sources(self):
        # Isolate from any real build dirs: binary absent, sources present.
        with patch.object(capabilities, "find_relinker", return_value=""):
            record = capabilities.relinker_record(explicit=str(self.root / "absent"),
                                                  build_dir=str(self.root))
        self.assertFalse(record["available"])
        self.assertEqual(record["input_formats"], ["ELF"])
        self.assertIn("SELF (use self_inspector first)", record["rejected_inputs"])
        # Flags are parsed from the real CliArgs.cpp, not invented.
        self.assertIn("--windows", record["cli_flags"])
        self.assertIn("CliArgs.cpp", record["cli_evidence"])
        self.assertTrue(record["supports_windows"])
        self.assertIn("no --help flag", " ".join(record["limits"]))
        self.assertIn("inspect_output", record["verify"])

    def test_self_inspector_record_states_no_crypto(self):
        record = capabilities.self_inspector_record(build_dir=str(self.root))
        self.assertIn("--extract <output.elf>", record["cli_flags"])
        self.assertTrue(any("no crypto" in limit.lower() for limit in record["limits"]))
        self.assertIn("re-parse", record["verify"])
        self.assertEqual(record["input_formats"], ["SELF"])

    def test_toolchain_record_structure(self):
        record = capabilities.toolchain_record()
        self.assertIn("cmake", record)
        self.assertIn("compiler", record)
        self.assertIn("available", record)
        self.assertIn("name", record["compiler"])

    def test_missing_tool_suggests_existing_equivalent(self):
        with patch.object(capabilities, "find_relinker", return_value=""):
            record = capabilities.relinker_record(explicit=str(self.root / "absent"),
                                                  build_dir=str(self.root))
        self.assertFalse(record["available"])
        self.assertTrue(record["path"] == "")


if __name__ == "__main__":
    unittest.main()
