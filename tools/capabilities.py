"""Single source of truth about tool capabilities in this checkout.

Every record carries: location, version, availability in this environment,
supported input/output formats, accepted CLI flags, required dependencies,
known limits, and how a successful result is verified.

CLI facts come from the real binary (usage probe) or the matching sources —
never invented. If a tool is absent, the record names the existing
in-project equivalent or the documented install step instead of guessing.
"""

import re
import shutil
import subprocess
import sys
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
ROOT = TOOLS_DIR.parent


def which_all(*names):
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    return ""


def run_capture(argv, timeout=10):
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        return proc.returncode, (proc.stdout or "") + (proc.stderr or "")
    except (OSError, subprocess.SubprocessError) as error:
        return None, f"cannot execute {' '.join(argv)}: {error}"


def git_commit():
    code, text = run_capture(["git", "rev-parse", "HEAD"], timeout=10)
    if code == 0 and re.fullmatch(r"[0-9a-f]{40}", text.strip()):
        return text.strip()
    return "unknown"


def find_relinker(explicit="", build_dir=""):
    """Locate an existing Relinker binary. Returns path string or ''."""
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    base_dirs = [Path(build_dir)] if build_dir else []
    base_dirs += [ROOT / "build", ROOT / "build-relinker"]
    for base in base_dirs:
        candidates.append(base / "core" / "relinker" / "relinker.exe")
        candidates.append(base / "core" / "relinker" / "relinker")
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    return which_all("relinker", "relinker.exe")


def relinker_cli_flags():
    """Authoritative Relinker flags parsed from CliArgs.cpp sources."""
    source = ROOT / "core" / "relinker" / "cli" / "src" / "CliArgs.cpp"
    try:
        content = source.read_text(encoding="utf-8")
    except OSError:
        return [], "CliArgs.cpp unreadable"
    flags = sorted(set(re.findall(r'"(--[a-z][a-z0-9-]*)"', content)))
    bare = sorted(set(re.findall(r'"(unused-filter=[0-9]\|[0-9]\|[0-9])"', content)))
    return flags + bare, f"parsed from {source}"


def relinker_supports_windows(relinker_path=""):
    """Check whether the actual Relinker binary/sources accept --windows.

    Probes the real binary usage text first; falls back to parsing
    core/relinker/cli/src/CliArgs.cpp. Returns (supported, evidence).
    """
    if relinker_path and Path(relinker_path).is_file():
        code, text = run_capture([relinker_path], timeout=15)
        if code is not None:
            if "--windows" in text:
                return True, f"{relinker_path} usage text advertises --windows"
            if text.strip():
                return False, f"{relinker_path} usage text has no --windows flag"
            # Empty probe output: fall through to the sources below.
    cli_source = ROOT / "core" / "relinker" / "cli" / "src" / "CliArgs.cpp"
    try:
        content = cli_source.read_text(encoding="utf-8")
    except OSError:
        return False, "no Relinker binary and CliArgs.cpp unreadable"
    if '"--windows"' in content:
        basis = "binary missing; " if not relinker_path else "binary probe inconclusive; "
        return True, basis + "CliArgs.cpp parses --windows"
    return False, "CliArgs.cpp has no --windows option"


def find_self_inspector(explicit="", build_dir=""):
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    base_dirs = [Path(build_dir)] if build_dir else []
    # Standalone CMake project (tools/self_inspector/CMakeLists.txt) builds
    # into build-self-inspector/ by convention (see PROMT.md workflow).
    base_dirs += [ROOT / "build", ROOT / "build-relinker", ROOT / "build-self-inspector"]
    for base in base_dirs:
        candidates.append(base / "tools" / "self_inspector" / "self_inspector.exe")
        candidates.append(base / "tools" / "self_inspector" / "self_inspector")
        candidates.append(base / "self_inspector.exe")
        candidates.append(base / "self_inspector")
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    return which_all("self_inspector", "self_inspector.exe")


def check_compiler():
    for name in ("g++", "clang++", "cl"):
        path = shutil.which(name)
        if path:
            code, text = run_capture([path, "--version"], timeout=10)
            first = (text.strip().splitlines() or ["unknown"])[0]
            return name, path, first
    return "", "", "no C++ compiler on PATH"


def relinker_record(explicit="", build_dir=""):
    """Capability record for the Relinker conversion tool."""
    path = find_relinker(explicit, build_dir)
    windows_supported, cli_evidence = relinker_supports_windows(path)
    flags, flags_evidence = relinker_cli_flags()
    return {
        "name": "relinker",
        "path": path,
        "available": bool(path),
        "version": f"source commit {git_commit()} (binary carries no --version flag)",
        "input_formats": ["ELF"],
        "rejected_inputs": ["SELF (use self_inspector first)", "PKG", "UNKNOWN", "TRUNCATED"],
        "output_formats": ["Linux ELF (default)", "Windows PE (only with --windows)"],
        "cli_flags": flags,
        "cli_evidence": f"{cli_evidence}; {flags_evidence}",
        "supports_windows": windows_supported,
        "dependencies": ["C++20 toolchain to build; built system libraries (*.prx) to run the game"],
        "limits": ["no --help flag (bare invocation prints usage)",
                   "exit 1 invalid arguments, exit 2 conversion failed",
                   "output defaults to Linux ELF regardless of filename"],
        "verify": "output exists; inspect_executable.inspect_output() matches the target format",
    }


def self_inspector_record(explicit="", build_dir=""):
    """Capability record for the project's SELF converter (plaintext only)."""
    path = find_self_inspector(explicit, build_dir)
    return {
        "name": "self_inspector",
        "path": path,
        "available": bool(path),
        "version": f"source commit {git_commit()}",
        "input_formats": ["SELF"],
        "output_formats": ["ELF (plaintext record remap only)", "JSON report"],
        "cli_flags": ["--report <report.json> (required)", "--extract <output.elf>"],
        "cli_evidence": "tools/self_inspector/src/main.cpp usage text",
        "dependencies": ["none beyond a C++17 build"],
        "limits": ["no crypto: encrypted records refuse conversion",
                   "never writes into the input; --extract target must be a separate path",
                   "analysis only; not a decryptor"],
        "verify": "re-parse the extracted file: ELF magic + valid headers (inspect_executable)",
    }


def toolchain_record():
    """Capability record for the build toolchain in this environment."""
    cmake = which_all("cmake")
    ninja = which_all("ninja")
    name, path, version = check_compiler()
    return {
        "name": "build_toolchain",
        "cmake": cmake, "ninja": ninja,
        "compiler": {"name": name, "path": path, "version": version},
        "available": bool(cmake and path),
        "dependencies": ["Windows: MinGW-w64 GCC 15.2.0 (docs/dev/BUILD.md); Linux: GCC/G++, binutils"],
        "limits": ["full build needs submodules (git submodule update --init --recursive); "
                   "relinker-only build does not"],
        "verify": "cmake configure succeeds; Relinker binary appears at build*/core/relinker/",
    }


def vulkan_record(timeout=30):
    """Capability record for the Vulkan runtime (delegates to vulkan_check)."""
    sys.path.insert(0, str(TOOLS_DIR))
    import vulkan_check
    report = vulkan_check.check(timeout)
    return {"name": "vulkan_runtime", **report,
            "verify": "loader present + at least one physical device enumerated"}


def registry(explicit_relinker="", explicit_inspector="", build_dir="", vulkan_timeout=30):
    """Whole capability registry for the current environment."""
    return {
        "relinker": relinker_record(explicit_relinker, build_dir),
        "self_inspector": self_inspector_record(explicit_inspector, build_dir),
        "build_toolchain": toolchain_record(),
        "vulkan_runtime": vulkan_record(vulkan_timeout),
        "python": {"name": "python", "path": sys.executable,
                   "version": sys.version.split()[0], "available": True},
    }


def main(argv=None):
    import argparse
    import json
    parser = argparse.ArgumentParser(description="Print the tool capability registry (JSON).")
    parser.add_argument("--relinker", default="")
    parser.add_argument("--build-dir", default="")
    args = parser.parse_args(argv)
    print(json.dumps(registry(args.relinker, "", args.build_dir), indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
