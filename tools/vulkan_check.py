"""Layered Vulkan diagnostics for AnyPS5 (read-only, no device created).

Checks each prerequisite separately so a failure names the exact layer:
  loader      Vulkan loader library / vulkaninfo presence
  devices     physical device enumeration (via ``vulkaninfo --summary``)
  features    graphics+compute queues, swapchain, Vulkan 1.1 (from summary text)
  logical     logical device creation — NOT attempted here (needs a GPU job);
              reported as ``not_attempted`` with the reason.
  shaders     shader compilation — owned by the shader recompiler tests.
  pipeline    pipeline creation — needs a device; not attempted here.
  present     presentation — needs a window/surface; not attempted here.

Anything that cannot be verified without real hardware execution is reported
as ``not_attempted`` with a reason, never as success. Unknown tools or
missing hardware produce evidence, not guesses.
"""

import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path


def loader_status():
    """Check the Vulkan loader without loading it into this process."""
    system = platform.system().lower()
    if system.startswith("win"):
        candidate = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "vulkan-1.dll"
        present = candidate.is_file()
        evidence = f"{candidate} {'exists' if present else 'missing'}"
    else:
        candidates = ("/usr/lib/x86_64-linux-gnu/libvulkan.so.1",
                      "/usr/lib/libvulkan.so.1",
                      "/usr/local/lib/libvulkan.so.1")
        found = next((c for c in candidates if Path(c).exists()), "")
        present = bool(found)
        evidence = f"loader at {found}" if found else f"none of {', '.join(candidates)} exists"
    info = shutil.which("vulkaninfo")
    return {"present": present or info is not None,
            "library": present, "vulkaninfo": info or "",
            "evidence": evidence + (f"; vulkaninfo at {info}" if info else "; vulkaninfo not on PATH")}


def run_vulkaninfo_summary(timeout=30):
    """Run ``vulkaninfo --summary`` as a subprocess array. Returns (code, text)."""
    exe = shutil.which("vulkaninfo")
    if not exe:
        return None, "vulkaninfo not on PATH: device enumeration unavailable"
    try:
        proc = subprocess.run([exe, "--summary"], capture_output=True, text=True, timeout=timeout)
        return proc.returncode, (proc.stdout or "") + (proc.stderr or "")
    except (OSError, subprocess.SubprocessError) as error:
        return None, f"cannot execute vulkaninfo: {error}"


def parse_devices(summary_text):
    """Extract physical device names and API versions from summary text."""
    devices = []
    current = None
    for line in summary_text.splitlines():
        name = re.match(r"\s*deviceName\s*=\s*(.+)", line)
        if name:
            current = {"name": name.group(1).strip(), "api": "", "layers": []}
            devices.append(current)
        api = re.match(r"\s*apiVersion\s*=\s*(.+)", line)
        if api and current is not None and not current["api"]:
            current["api"] = api.group(1).strip()
    return devices


def check(timeout=30):
    """Full layered check. Returns a plain dict (JSON-serializable)."""
    report = {"layers": {}, "ok": False, "summary": ""}
    loader = loader_status()
    report["layers"]["loader"] = {
        "status": "present" if loader["present"] else "missing",
        "evidence": loader["evidence"],
    }
    code, text = run_vulkaninfo_summary(timeout)
    if code is None:
        report["layers"]["devices"] = {"status": "unknown", "evidence": text, "devices": []}
        report["layers"]["features"] = {"status": "unknown",
                                        "evidence": "no device list; cannot assess queues/swapchain/Vulkan 1.1"}
    elif code != 0:
        report["layers"]["devices"] = {"status": "failed",
                                       "evidence": f"vulkaninfo --summary exit {code}: {text[:2000]}",
                                       "devices": []}
        report["layers"]["features"] = {"status": "unknown",
                                        "evidence": "device enumeration failed"}
    else:
        devices = parse_devices(text)
        report["layers"]["devices"] = {
            "status": "present" if devices else "missing",
            "evidence": f"{len(devices)} physical device(s): "
                        + (", ".join(d["name"] for d in devices) if devices else "none enumerated"),
            "devices": devices,
        }
        has_11 = bool(re.search(r"1\.[1-9]", text))
        report["layers"]["features"] = {
            "status": "likely" if devices and has_11 else "unknown",
            "evidence": ("Vulkan 1.1+ mentioned and devices present; queue families / "
                         "swapchain support require device creation, see note"
                         if devices and has_11 else
                         "cannot confirm graphics+compute queues and swapchain without device creation"),
        }
    for layer, reason in (
            ("logical", "creating a VkDevice is a GPU job owned by the game/tests, not by diagnostics"),
            ("shaders", "shader compilation is owned by core/shader/recompiler and its tests"),
            ("pipeline", "pipeline creation needs a logical device; not attempted here"),
            ("present", "presentation needs a window surface; not attempted here")):
        report["layers"][layer] = {"status": "not_attempted", "evidence": reason}
    loader_ok = report["layers"]["loader"]["status"] == "present"
    devices_ok = report["layers"]["devices"].get("status") == "present"
    report["ok"] = loader_ok and devices_ok
    if report["ok"]:
        report["summary"] = "loader and physical devices present; launch may proceed to RUNTIME_START"
    elif not loader_ok:
        report["summary"] = "Vulkan loader missing: game launch (RUNTIME_START) is blocked"
    else:
        report["summary"] = "no usable physical device enumerated: game launch (RUNTIME_START) is blocked"
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description="Layered Vulkan diagnostics (read-only).")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--timeout", type=int, default=30)
    args = parser.parse_args(argv)
    report = check(args.timeout)
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        for layer, info in report["layers"].items():
            print(f"{layer:10s} {info['status']:13s} {info['evidence'][:160]}")
        print(f"summary: {report['summary']}")
    return 0 if report["ok"] else 2


if __name__ == "__main__":
    sys.exit(main())
