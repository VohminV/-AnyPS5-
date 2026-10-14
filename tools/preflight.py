"""Mandatory environment preflight for AnyPS5 Relinker workflows.

Determines the real host/target configuration and prints a structured report
with one of: READY, READY_WITH_WARNINGS, BLOCKED_ENVIRONMENT,
BLOCKED_INPUT_FORMAT, BLOCKED_DEPENDENCY, BLOCKED_BUILD.

Host OS is detected with ``platform.system()`` (never from a ``.exe``
suffix, terminal name or error text) and is kept strictly separate from the
Relinker *target* OS, which comes only from ``--target`` (default: linux).

The actual Relinker CLI is probed (usage text of the real binary, falling
back to ``core/relinker/cli/src/CliArgs.cpp`` sources), because this
checkout's Relinker has no ``--help`` flag: invoking it without arguments
prints the usage syntax. ``--windows`` is therefore selected from the target,
never by trial and error.

Example:
  python tools/preflight.py --target windows --game-dir "D:\\Games\\MyGame"
"""

import argparse
import hashlib
import json
import os
import platform
import shlex
import shutil
import struct
import subprocess
import sys
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
ROOT = TOOLS_DIR.parent
sys.path.insert(0, str(TOOLS_DIR))
import inspect_executable as inspect_mod  # noqa: E402
import vulkan_check as vulkan_check_mod  # noqa: E402
# Tool capability registry (single source of truth; re-exported here so that
# existing callers and tests keep working against the preflight namespace).
from capabilities import (  # noqa: E402
    find_relinker,
    find_self_inspector,
    relinker_supports_windows,
    check_compiler,
    which_all,
    run_capture,
)

STATUS_READY = "READY"
STATUS_READY_WARNINGS = "READY_WITH_WARNINGS"
STATUS_BLOCKED_ENV = "BLOCKED_ENVIRONMENT"
STATUS_BLOCKED_INPUT = "BLOCKED_INPUT_FORMAT"
STATUS_BLOCKED_DEP = "BLOCKED_DEPENDENCY"
STATUS_BLOCKED_BUILD = "BLOCKED_BUILD"

TARGETS = ("windows", "linux")
DEFAULT_TARGET = "linux"  # docs/user/USAGE.md: output defaults to Linux ELF.
GUARD_FILE = ROOT / "logs" / ".last_failed_command.json"
INPUT_CANDIDATES = ("eboot.bin", "input.elf", "app.elf")


# ---------------------------------------------------------------- host/guild

def detect_host(system=None, machine=None, env=None):
    """Detect host OS/arch/shell from the OS API, not from file names."""
    system = system if system is not None else platform.system()
    machine = machine if machine is not None else platform.machine()
    env = env if env is not None else os.environ
    lowered = system.lower()
    if lowered.startswith("win"):
        host_os = "Windows"
    elif lowered.startswith("linux"):
        host_os = "Linux"
    elif lowered.startswith("darwin"):
        host_os = "macOS"
    else:
        host_os = "unsupported"
    arch = (machine or "").lower()
    if arch in ("x86_64", "amd64"):
        host_arch = "x86_64"
    elif arch in ("aarch64", "arm64"):
        host_arch = "arm64"
    else:
        host_arch = machine or "unknown"
    shell = "unknown"
    if host_os == "Windows":
        # PSModulePath exists under both cmd and PowerShell, so it cannot
        # distinguish them; report honestly instead of guessing.
        if env.get("PWSH_VERSION") or env.get("PSVERSION"):
            shell = "PowerShell"
        elif env.get("PROMPT") or env.get("COMSPEC"):
            shell = "cmd/PowerShell (ambiguous)"
    else:
        if env.get("BASH_VERSION"):
            shell = "bash"
        elif env.get("ZSH_VERSION"):
            shell = "zsh"
        elif env.get("SHELL"):
            shell = str(env.get("SHELL"))
    return {"host_os": host_os, "host_arch": host_arch, "shell": shell,
            "system": system, "machine": machine}


def git_info(repo_root):
    """Best-effort branch/dirty status. Read-only; absence of git is not fatal."""
    info = {"branch": "unknown", "dirty": "unknown", "commit": "unknown"}
    try:
        branch = subprocess.run(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"],
            capture_output=True, text=True, timeout=10, cwd=str(repo_root))
        if branch.returncode == 0:
            info["branch"] = branch.stdout.strip()
        commit = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            capture_output=True, text=True, timeout=10, cwd=str(repo_root))
        if commit.returncode == 0:
            info["commit"] = commit.stdout.strip()
        status = subprocess.run(
            ["git", "status", "--short"],
            capture_output=True, text=True, timeout=10, cwd=str(repo_root))
        if status.returncode == 0:
            info["dirty"] = "dirty" if status.stdout.strip() else "clean"
    except (OSError, subprocess.SubprocessError):
        pass
    return info


# ------------------------------------------------------------- build config

def read_build_config(build_dir):
    cache = Path(build_dir) / "CMakeCache.txt"
    config = {"dir": str(build_dir), "configured": False}
    if not cache.is_file():
        return config
    try:
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("CMAKE_BUILD_TYPE:"):
                config["build_type"] = line.split("=", 1)[1]
            elif line.startswith("CMAKE_CXX_COMPILER:"):
                config["cxx_compiler"] = line.split("=", 1)[1]
            elif line.startswith("ANYPS5_RELINKER_ONLY:"):
                config["relinker_only"] = line.split("=", 1)[1]
    except OSError:
        return config
    config["configured"] = True
    return config


def check_vulkan():
    """Layered Vulkan report (delegates to vulkan_check).

    Missing runtime only warns here; it blocks RUNTIME_START, not relink.
    Keeps the legacy ``present``/``detail`` keys for compatibility.
    """
    report = vulkan_check_mod.check()
    report["present"] = report["ok"]
    report["detail"] = report["summary"]
    return report


def check_libs(target, build_dir=""):
    """Check AnyPS5 system libraries built for the target OS."""
    bases = [Path(build_dir)] if build_dir else []
    bases += [ROOT / "build", ROOT / "build-relinker"]
    for base in bases:
        libs = base / "core" / "libs" / "libs"
        if libs.is_dir() and list(libs.glob("*.prx")):
            return {"ready": True, "dir": str(libs), "count": len(list(libs.glob("*.prx")))}
    return {"ready": False, "dir": "", "count": 0}


# ------------------------------------------------------------ command guard

def command_hash(argv):
    return hashlib.sha256(json.dumps(argv).encode()).hexdigest()[:16]


def is_repeat_command(argv, guard_file=None):
    """True if this exact argv already failed (prevents blind retries)."""
    guard_file = guard_file or GUARD_FILE
    try:
        data = json.loads(Path(guard_file).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    return data.get("hash") == command_hash(list(argv))


def record_failure(argv, context="", guard_file=None):
    guard_file = guard_file or GUARD_FILE
    try:
        Path(guard_file).parent.mkdir(parents=True, exist_ok=True)
        Path(guard_file).write_text(
            json.dumps({"hash": command_hash(list(argv)), "argv": list(argv),
                        "context": context}, indent=2), encoding="utf-8")
    except OSError:
        pass


def format_next_command(argv, host_os):
    """Quote argv for display in the host shell. Execution uses the array."""
    if host_os == "Windows":
        return subprocess.list2cmdline(list(argv))
    return shlex.join(list(argv))


# ------------------------------------------------------------------ main

def build_relink_argv(relinker, input_path, output_path, target,
                      windows_supported, extra_args=()):
    """Single place where --windows is decided: from target, never by retry."""
    if target == "windows":
        if not windows_supported:
            return None, "target is windows but this Relinker has no --windows flag"
        return [relinker, "--windows", *extra_args, str(input_path), str(output_path)], ""
    if target == "linux":
        return [relinker, *extra_args, str(input_path), str(output_path)], ""
    return None, f"target '{target}' is not supported by this Relinker version"


def build_command_for_host(host_os):
    if host_os == "Windows":
        return ("cmake -S . -B build-relinker -G Ninja -DCMAKE_BUILD_TYPE=Release "
                "-DANYPS5_RELINKER_ONLY=ON -DBUILD_TESTING=ON && "
                "cmake --build build-relinker --parallel")
    return ("cmake -S . -B build-relinker -G Ninja -DCMAKE_BUILD_TYPE=Release "
            "-DANYPS5_RELINKER_ONLY=ON -DBUILD_TESTING=ON && "
            "cmake --build build-relinker --parallel")


def preflight(target=DEFAULT_TARGET, game_dir="", input_path="", output_path="",
              relinker_path="", build_dir="", extra_relink_args=(),
              host_override=None):
    """Run all checks. Returns (report_dict, status)."""
    host = detect_host(**(host_override or {}))
    host_os = host["host_os"]
    report = {
        "host_os": host_os, "host_arch": host["host_arch"], "shell": host["shell"],
        "os_version": platform.version(),
        "repo_root": str(ROOT), "cwd": os.getcwd(),
        "git": git_info(ROOT),
        "target": target,
        "warnings": [], "blockers": [],
        "next_action": "",
    }

    def block(status, reason, evidence, action):
        report["blockers"].append({"status": status, "reason": reason,
                                   "evidence": evidence, "next": action})

    if target not in TARGETS:
        block(STATUS_BLOCKED_ENV, f"unsupported target '{target}'",
              "TARGETS = ('windows', 'linux') in tools/preflight.py",
              "Pass --target windows or --target linux.")
        report["status"] = STATUS_BLOCKED_ENV
        return report, STATUS_BLOCKED_ENV
    if host_os == "unsupported":
        block(STATUS_BLOCKED_ENV, f"unsupported host OS '{host['system']}'",
              f"platform.system() = {host['system']!r}",
              "Run preflight on Windows or Linux.")
    if host["host_arch"] not in ("x86_64", "arm64"):
        report["warnings"].append(f"unusual host arch '{host['machine']}'; x86_64 is required to run converted games")

    report["python"] = sys.executable
    report["cmake"] = which_all("cmake")
    report["ninja"] = which_all("ninja")
    compiler_name, compiler_path, compiler_version = check_compiler()
    report["compiler"] = {"name": compiler_name, "path": compiler_path, "version": compiler_version}
    if not report["cmake"]:
        block(STATUS_BLOCKED_DEP, "CMake not found on PATH", "shutil.which('cmake') is empty",
              "Install CMake 3.22.1+ and re-run preflight.")
    if not compiler_path:
        block(STATUS_BLOCKED_DEP, "no C++ compiler on PATH",
              "g++/clang++/cl all missing; Windows requires MinGW-w64 GCC 15.2.0 (see docs/dev/BUILD.md)",
              "Install the documented toolchain and re-run preflight.")
    report["builds"] = [read_build_config(d) for d in
                        ([str(Path(build_dir))] if build_dir else
                         [str(ROOT / "build"), str(ROOT / "build-relinker")])]

    relinker = find_relinker(relinker_path, build_dir)
    report["relinker"] = relinker
    windows_supported, cli_evidence = relinker_supports_windows(relinker)
    report["relinker_supports_windows"] = windows_supported
    report["relinker_cli_evidence"] = cli_evidence
    if not relinker:
        block(STATUS_BLOCKED_BUILD, "Relinker binary not found",
              "no build/core/relinker/relinker(.exe), build-relinker/..., or PATH entry",
              "Build it first: " + build_command_for_host(host_os if host_os != "unsupported" else "Linux"))

    vulkan = check_vulkan()
    report["vulkan"] = vulkan
    if not vulkan["present"]:
        report["warnings"].append(
            f"Vulkan runtime not detected ({vulkan['detail']}); relink works, game launch (RUNTIME_START) will fail")

    # ---- inputs
    game_root = Path(game_dir) if game_dir else None
    if game_root and not game_root.is_dir():
        block(STATUS_BLOCKED_INPUT, f"game directory not found: {game_root}",
              f"Path({game_dir!r}).is_dir() is False", "Pass an existing --game-dir.")
        game_root = None
    resolved_input = Path(input_path) if input_path else None
    if resolved_input is None and game_root is not None:
        for name in INPUT_CANDIDATES:
            if (game_root / name).is_file():
                resolved_input = game_root / name
                break
    report["input"] = str(resolved_input) if resolved_input else ""
    file_info = inspect_mod.inspect_file(resolved_input) if resolved_input else None
    report["input_inspection"] = file_info
    if resolved_input is None:
        block(STATUS_BLOCKED_INPUT, "no input executable given or found",
              f"searched {', '.join(INPUT_CANDIDATES)} in {game_root or '(no game dir)'}",
              "Pass --input <clean ELF> or put eboot.bin/input.elf in --game-dir.")
    elif file_info["format"] == "SELF":
        inspector = find_self_inspector("", build_dir)
        report["self_inspector"] = inspector
        if inspector:
            report["warnings"].append(
                "SELF input: supported conversion path exists via self_inspector "
                f"({inspector}) plaintext remap into a SEPARATE directory; original is preserved. "
                "No crypto is implemented: encrypted records still block conversion.")
            report["next_action"] = (
                f"Convert first: {format_next_command([inspector, str(resolved_input), '--report', 'self.json', '--extract', 'extracted/input.elf'], host_os)} "
                "then re-run preflight with --input extracted/input.elf")
        else:
            block(STATUS_BLOCKED_INPUT,
                  "SELF input requires a clean ELF; no supported converter is available",
                  f"{file_info['head_hex']} ({file_info['format']}); "
                  "self_inspector binary not found in build*/tools/self_inspector/ or PATH; "
                  "third-party downloaders and invented CLIs are not allowed",
                  "Provide a clean ELF dump as input, or build self_inspector and extract "
                  "to a separate directory, then re-run preflight on the extracted ELF.")
    elif not file_info["relinkable"]:
        block(STATUS_BLOCKED_INPUT, f"input is not a clean ELF: {file_info['reason']}",
              f"head {file_info['head_hex']}, size {file_info['size']}",
              "Provide a clean ELF64 little-endian x86-64 executable.")

    modules_report = None
    if game_root is not None:
        modules_report = inspect_mod.inspect_game_dir(game_root, ())
        report["modules"] = {k: v for k, v in modules_report.items() if k != "modules"}
        report["modules"]["files"] = len(modules_report["modules"])
        if modules_report["error"]:
            block(STATUS_BLOCKED_INPUT, modules_report["error"],
                  f"dirs beside input: {modules_report['dirs_found'] or 'none'}",
                  "Fix the game directory layout per docs/user/USAGE.md (sce_module XOR sce_modules, prx may coexist).")
    libs = check_libs(target, build_dir)
    report["libs"] = libs
    if not libs["ready"]:
        report["warnings"].append(
            "built system libraries (*.prx for the target OS) not found; "
            "run: cmake --build build --target libs")

    # ---- target argv (single decision point, no trial runs)
    report["relink_argv"] = []
    if resolved_input is not None and file_info and file_info["relinkable"]:
        out = Path(output_path) if output_path else None
        if out is None and game_root is not None:
            out = game_root / ("app.exe" if target == "windows" else "app.elf")
        report["output"] = str(out) if out else ""
        if out is not None:
            if relinker:
                argv, error = build_relink_argv(
                    relinker, resolved_input, out, target, windows_supported, extra_relink_args)
                if error:
                    block(STATUS_BLOCKED_BUILD, error, cli_evidence,
                          "Use --target linux, or rebuild Relinker from sources that support --windows.")
                else:
                    report["relink_argv"] = argv
                    if is_repeat_command(argv):
                        report["warnings"].append(
                            "this exact Relinker command already failed (see logs/.last_failed_command.json); "
                            "change inputs or parameters before retrying instead of repeating it")
                    report["next_action"] = report.get("next_action") or \
                        f"Relink: {format_next_command(argv, host_os if host_os != 'unsupported' else 'Linux')}"
            if (resolved_input is not None and out is not None
                    and Path(resolved_input) == Path(out)):
                block(STATUS_BLOCKED_INPUT, "output would overwrite the input file",
                      f"input == output == {out}",
                      "Choose an --output path different from the input; originals are never overwritten.")
    if not report.get("next_action") and not report["blockers"] and not relinker:
        report["next_action"] = "Build it first: " + build_command_for_host(
            host_os if host_os != "unsupported" else "Linux")

    if report["blockers"]:
        # Most severe first: environment > input > dependency > build.
        order = {STATUS_BLOCKED_ENV: 0, STATUS_BLOCKED_INPUT: 1,
                 STATUS_BLOCKED_DEP: 2, STATUS_BLOCKED_BUILD: 3}
        status = sorted((b["status"] for b in report["blockers"]), key=order.__getitem__)[0]
    elif report["warnings"]:
        status = STATUS_READY_WARNINGS
    else:
        status = STATUS_READY
    report["status"] = status
    # Explicit split: what builds here vs what runs the result.
    report["build_toolchain"] = {
        "cmake": report.get("cmake"), "ninja": report.get("ninja"),
        "compiler": report.get("compiler"), "builds": report.get("builds"),
        "relinker": report.get("relinker"),
    }
    report["runtime_environment"] = {
        "target": target,
        "vulkan": {k: v for k, v in report.get("vulkan", {}).items()
                   if k in ("ok", "summary", "layers")},
        "libs": report.get("libs"),
        "output": report.get("output", ""),
    }
    return report, status


def print_human(report):
    print(f"host_os:   {report['host_os']}  target: {report['target']}")
    print(f"host_arch: {report['host_arch']}  shell: {report['shell']}")
    print(f"os_version: {report.get('os_version', 'unknown')}")
    print(f"repo_root: {report['repo_root']}")
    print(f"cwd:       {report['cwd']}")
    git = report["git"]
    print(f"git:       branch={git['branch']} status={git['dirty']} commit={git['commit'][:12]}")
    print(f"python:    {report['python']}")
    print(f"cmake:     {report.get('cmake') or 'NOT FOUND'}")
    print(f"ninja:     {report.get('ninja') or 'NOT FOUND'}")
    compiler = report.get("compiler", {})
    print(f"compiler:  {compiler.get('path') or 'NOT FOUND'} ({compiler.get('version', '')})")
    for build in report.get("builds", []):
        print(f"build:     {build['dir']} configured={build['configured']} "
              f"{ {k: v for k, v in build.items() if k not in ('dir', 'configured') } }")
    print(f"relinker:  {report.get('relinker') or 'NOT FOUND'}")
    print(f"  --windows support: {report.get('relinker_supports_windows')} ({report.get('relinker_cli_evidence')})")
    vulkan = report.get("vulkan", {})
    print(f"vulkan:    {'present' if vulkan.get('present') else 'NOT DETECTED'} ({vulkan.get('detail')})")
    for layer, info in (vulkan.get("layers") or {}).items():
        print(f"  {layer:10s} {info.get('status', '?'):13s} {str(info.get('evidence', ''))[:120]}")
    info = report.get("input_inspection")
    if info:
        print(f"input:     {info['path']}")
        print(f"  size/format: {info['size']} bytes / {info['format']} [{info['head_hex']}]")
        print(f"  verdict: {info['reason']}")
    modules = report.get("modules")
    if modules:
        print(f"modules:   dirs={modules.get('dirs_found') or 'none'} elf={modules.get('elf_modules')} files={modules.get('files')}")
        if modules.get("error"):
            print(f"  error: {modules['error']}")
    libs = report.get("libs", {})
    print(f"libs:      {'ready' if libs.get('ready') else 'NOT READY'} "
          f"({libs.get('count', 0)} .prx in {libs.get('dir') or 'no build libs dir'})")
    if report.get("relink_argv"):
        print(f"relink:    {' '.join(repr(a) if ' ' in a else a for a in report['relink_argv'])}")
    for warning in report.get("warnings", []):
        print(f"WARNING: {warning}")
    for blocker in report.get("blockers", []):
        print(f"BLOCKER [{blocker['status']}]: {blocker['reason']}")
        print(f"  evidence: {blocker['evidence']}")
        print(f"  next: {blocker['next']}")
    if report.get("next_action"):
        print(f"next: {report['next_action']}")
    print(f"STATUS: {report['status']}")


def main(argv=None):
    parser = argparse.ArgumentParser(description="AnyPS5 environment preflight (host/target aware).")
    parser.add_argument("--target", default=DEFAULT_TARGET, choices=list(TARGETS),
                        help="relink target OS (default: linux = Linux ELF output)")
    parser.add_argument("--game-dir", default="", help="game directory (input + sce_module/sce_modules/prx)")
    parser.add_argument("--input", default="", help="input executable (default: eboot.bin/input.elf in --game-dir)")
    parser.add_argument("--output", default="", help="relink output path (default: app.elf/app.exe in --game-dir)")
    parser.add_argument("--relinker", default="", help="explicit Relinker binary path")
    parser.add_argument("--build-dir", default="", help="explicit build directory to probe")
    parser.add_argument("--json", action="store_true", help="print machine-readable JSON report")
    parser.add_argument("--relink-arg", action="append", default=[],
                        help="extra arg forwarded to Relinker (repeatable, e.g. --relink-arg=--to-intel)")
    args = parser.parse_args(argv)
    report, status = preflight(
        target=args.target, game_dir=args.game_dir, input_path=args.input,
        output_path=args.output, relinker_path=args.relinker, build_dir=args.build_dir,
        extra_relink_args=tuple(args.relink_arg))
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print_human(report)
    return 0 if status in (STATUS_READY, STATUS_READY_WARNINGS) else 2


if __name__ == "__main__":
    sys.exit(main())
