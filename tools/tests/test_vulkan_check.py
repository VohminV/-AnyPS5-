"""Tests for tools/vulkan_check.py (synthetic summaries, no GPU needed)."""

import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import vulkan_check

SUMMARY = """
Vulkan Instance Version: 1.3.280
deviceName = Fake GPU 1
apiVersion = 1.3.280
deviceName = Fake GPU 2
apiVersion = 1.1.0
"""


class VulkanCheckTests(unittest.TestCase):
    def test_parse_devices_from_summary(self):
        devices = vulkan_check.parse_devices(SUMMARY)
        self.assertEqual([d["name"] for d in devices], ["Fake GPU 1", "Fake GPU 2"])
        self.assertEqual(devices[1]["api"], "1.1.0")

    def test_missing_vulkaninfo_is_unknown_not_success(self):
        with patch.object(vulkan_check.shutil, "which", return_value=None):
            with patch.object(vulkan_check, "loader_status",
                              return_value={"present": False, "evidence": "no loader"}):
                report = vulkan_check.check()
        self.assertFalse(report["ok"])
        self.assertEqual(report["layers"]["devices"]["status"], "unknown")
        self.assertIn("RUNTIME_START", report["summary"])

    def test_layers_never_claim_unattempted_work(self):
        with patch.object(vulkan_check, "run_vulkaninfo_summary",
                          return_value=(0, SUMMARY)):
            report = vulkan_check.check()
        for layer in ("logical", "shaders", "pipeline", "present"):
            self.assertEqual(report["layers"][layer]["status"], "not_attempted")
        self.assertEqual(len(report["layers"]["devices"]["devices"]), 2)
        self.assertTrue(report["ok"])

    def test_failed_enumeration_blocks_launch(self):
        with patch.object(vulkan_check, "run_vulkaninfo_summary",
                          return_value=(1, "ERROR: no devices")):
            report = vulkan_check.check()
        self.assertFalse(report["ok"])
        self.assertEqual(report["layers"]["devices"]["status"], "failed")

    def test_all_layers_present(self):
        report = vulkan_check.check()
        self.assertEqual(set(report["layers"]),
                         {"loader", "devices", "features", "logical",
                          "shaders", "pipeline", "present"})
        for layer in ("loader", "devices", "features"):
            self.assertIn(report["layers"][layer]["status"],
                          {"present", "missing", "failed", "unknown", "likely"})


if __name__ == "__main__":
    unittest.main()
