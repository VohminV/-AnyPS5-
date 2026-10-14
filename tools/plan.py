"""Processing planner for AnyPS5 game preparation and launch.

Builds an execution plan from facts, not guesses:
  1. task requirements (prepare only, or prepare + run),
  2. actual input state (tools/inspect_executable.py),
  3. available tools (tools/capabilities.py registry),
  4. stage dependencies,
  5. target platform,
  6. verifiable pre/postconditions.

No stage is started unless its preconditions hold. A stage is not passed on
exit code 0 alone: expected artifacts are verified (existence, format, size,
validity). On failure the plan names the localized stage and cause.

Stage names match tools/run_game.py: PREFLIGHT, RELINK, RUNTIME_START,
GAME_RUNNING. The SELF conversion is a RELINK sub-step (CONVERT_SELF).
"""

import sys
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_DIR))
import preflight as preflight_mod  # noqa: E402
import inspect_executable as inspect_mod  # noqa: E402


def _stage(name, goal, inputs, preconditions, tool, command, artifacts,
           success, timeout, verify, on_error):
    return {"name": name, "goal": goal, "inputs": inputs,
            "preconditions": preconditions, "tool": tool, "command": command,
            "expected_artifacts": artifacts, "success_criteria": success,
            "timeout": timeout, "verify": verify, "on_error": on_error}


def build_plan(target="linux", game_dir="", input_path="", output_path="",
               relinker_path="", build_dir="", extra_relink_args=(),
               want_run=False, relink_timeout=300, run_timeout=30,
               host_override=None):
    """Build the plan. Returns (plan_dict, first_block)."""
    report, status = preflight_mod.preflight(
        target=target, game_dir=game_dir, input_path=input_path,
        output_path=output_path, relinker_path=relinker_path,
        build_dir=build_dir, extra_relink_args=tuple(extra_relink_args),
        host_override=host_override)
    host_os = report["host_os"]
    stages = []
    game = str(Path(game_dir)) if game_dir else ""

    preflight_argv = [sys.executable, str(TOOLS_DIR / "preflight.py"),
                      "--target", target]
    if game_dir:
        preflight_argv += ["--game-dir", game_dir]
    if input_path:
        preflight_argv += ["--input", input_path]
    if output_path:
        preflight_argv += ["--output", output_path]
    stages.append(_stage(
        "PREFLIGHT", "verify environment, inputs and tool capabilities",
        {"game_dir": game}, ["none: diagnostics always runnable"],
        "tools/preflight.py", preflight_argv, ["preflight report"],
        "STATUS is READY or READY_WITH_WARNINGS", 120,
        "parse STATUS line; blocked statuses stop the plan",
        "fix the reported blocker (reason/evidence/next), then re-plan"))

    info = report.get("input_inspection")
    if info and info["format"] == "SELF":
        inspector = report.get("self_inspector") or ""
        out_dir = (Path(game_dir) / "_anyps5_extracted") if game_dir else (preflight_mod.ROOT / "_anyps5_extracted")
        extracted = out_dir / "input.elf"
        stages.append(_stage(
            "CONVERT_SELF", "obtain clean ELFs from SELF without touching the originals",
            {"input": report.get("input"), "game_dir": game,
             "note": "main executable plus SELF modules in sce_module/sce_modules/prx "
                     "are mirrored into <game-dir>/_anyps5_extracted/"},
            ["input format is SELF",
             "self_inspector available" if inspector else "MISSING: self_inspector available"],
            "self_inspector",
            ([inspector, report["input"], "--report", str(out_dir / "self.json"),
              "--extract", str(extracted)] if inspector else []),
            [str(extracted), str(out_dir / "self.json")],
            "extracted file exists and re-parses as clean ELF (magic + headers + arch)",
            300, "inspect_executable.inspect_file() on the extracted file",
            "encrypted records or invalid embedded ELF: provide a clean ELF dump instead"))
        effective_input = str(extracted)
    else:
        effective_input = report.get("input", "")

    relink_argv = list(report.get("relink_argv", []))
    if effective_input and relink_argv:
        # Point the pre-computed argv at the (possibly converted) input.
        relink_argv = [effective_input if a == report.get("input") else a for a in relink_argv]
    input_ok = bool(info and info["relinkable"])
    # SELF with an available converter: RELINK describes the post-conversion
    # state; run_game executes CONVERT_SELF first and then re-plans, so the
    # missing clean ELF must not block the plan here.
    pending_convert = bool(info and info["format"] == "SELF"
                           and (report.get("self_inspector") or ""))
    stages.append(_stage(
        "RELINK", f"convert clean ELF to {target} format",
        {"input": effective_input, "game_dir": game},
        ["input is a clean relinkable ELF" if input_ok
         else ("pending CONVERT_SELF output" if pending_convert
               else "MISSING: input is a clean relinkable ELF (SELF must convert first)"),
         "Relinker binary available" if report.get("relinker") else "MISSING: Relinker binary available",
         "target supported by this Relinker" if relink_argv
         else ("target check deferred until CONVERT_SELF" if pending_convert
               else ("MISSING: target supported by this Relinker" if report.get("relinker")
                     else "target check deferred: no Relinker binary")),
         "output differs from input",
         "command is not an already-failed identical retry"],
        "relinker", relink_argv, [report.get("output", "")],
        "exit 0 AND output exists AND format matches target (PE/MZ for windows, ELF for linux)",
        relink_timeout,
        "inspect_executable.inspect_output(); record command hash on failure",
        "record failure in logs/.last_failed_command.json; change inputs/params, never blind-repeat"))

    stages.append(_stage(
        "RUNTIME_START", "verify the prepared game can start",
        {"output": report.get("output", "")},
        ["RELINK artifacts verified",
         "system libraries (*.prx) built for the target" if (report.get("libs") or {}).get("ready")
         else "MISSING: system libraries (*.prx) built for the target",
         "Vulkan loader + physical device present" if (report.get("vulkan") or {}).get("ok")
         else "MISSING: Vulkan loader + physical device present"],
        "launcher", [report.get("output", "")] if report.get("output") else [],
        [f"launch log logs/run_*.log"],
        "process starts; bounded wait only (timeout means: still running, not hung)",
        run_timeout if want_run else 0,
        "exit code + log; 124 = still running at timeout",
        "missing libs: cmake --build build --target libs; no device: see vulkan_check layers"))
    if want_run:
        stages.append(_stage(
            "GAME_RUNNING", "run the converted game under a timeout",
            {"output": report.get("output", "")},
            ["RUNTIME_START preconditions hold"],
            "converted game", [report.get("output", "")] if report.get("output") else [],
            ["game process exit code"],
            f"exit code observed within {run_timeout}s (124 = still running at timeout)",
            run_timeout, "logs/run_*.log with stdout/stderr",
            "see the log: loader errors point at libs layout, device errors at vulkan_check"))
    else:
        stages.append(_stage(
            "GAME_RUNNING", "skipped: preparation only (no --run)",
            {}, ["--run was not requested"], "none", [],
            [], "not applicable", 0, "not applicable",
            "pass --run to include the launch stage"))

    first_block = None
    for stage in stages:
        missing = [p for p in stage["preconditions"] if p.startswith("MISSING:")]
        if missing:
            first_block = {"stage": stage["name"], "reason": "; ".join(missing),
                           "evidence": f"preflight STATUS={status}",
                           "next": report.get("next_action", "")}
            break
        if stage["name"] in ("RELINK", "RUNTIME_START", "GAME_RUNNING") and status not in (
                preflight_mod.STATUS_READY, preflight_mod.STATUS_READY_WARNINGS):
            # PREFLIGHT itself failed: downstream stages cannot start.
            if stage["name"] == "RELINK":
                first_block = {"stage": stage["name"],
                               "reason": f"preflight is {status}",
                               "evidence": "; ".join(b["reason"] for b in report.get("blockers", [])),
                               "next": report.get("next_action", "")}
                break
    plan = {"target": target, "host_os": host_os, "want_run": want_run,
            "preflight_status": status, "stages": stages,
            "attempt_log": "logs/.last_failed_command.json",
            "run_log_pattern": "logs/run_*.log"}
    return plan, first_block


def main(argv=None):
    import argparse
    import json
    parser = argparse.ArgumentParser(description="Print the processing plan (JSON, no execution).")
    parser.add_argument("--target", default="linux", choices=["windows", "linux"])
    parser.add_argument("--game-dir", default="")
    parser.add_argument("--input", default="")
    parser.add_argument("--output", default="")
    parser.add_argument("--relinker", default="")
    parser.add_argument("--build-dir", default="")
    parser.add_argument("--run", action="store_true")
    args = parser.parse_args(argv)
    plan, blocked = build_plan(target=args.target, game_dir=args.game_dir,
                               input_path=args.input, output_path=args.output,
                               relinker_path=args.relinker, build_dir=args.build_dir,
                               want_run=args.run)
    print(json.dumps({"plan": plan, "blocked": blocked}, indent=2))
    return 0 if blocked is None else 2


if __name__ == "__main__":
    sys.exit(main())
