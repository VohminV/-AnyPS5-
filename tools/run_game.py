"""Single prepare-and-launch scenario for AnyPS5 games (Windows + Linux).

Executes the plan built by tools/plan.py. Stages (never conflated):
PREFLIGHT -> RELINK (-> CONVERT_SELF when the input is SELF) ->
RUNTIME_START -> GAME_RUNNING.
A successful conversion is NOT reported as a successful game launch.

Usage:
  python tools/run_game.py --game-dir "D:\\Games\\MyGame" --target windows
  python tools/run_game.py --game-dir "D:\\Games\\MyGame" --target windows --run
"""

import argparse
import datetime
import subprocess
import sys
import time
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
ROOT = TOOLS_DIR.parent
sys.path.insert(0, str(TOOLS_DIR))
import inspect_executable as inspect_mod  # noqa: E402
import preflight as preflight_mod  # noqa: E402
import plan as plan_mod  # noqa: E402

STAGE_PREFLIGHT = "PREFLIGHT"
STAGE_RELINK = "RELINK"
STAGE_RUNTIME_START = "RUNTIME_START"
STAGE_GAME_RUNNING = "GAME_RUNNING"


def log_path():
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    path = ROOT / "logs" / f"run_{stamp}.log"
    path.parent.mkdir(parents=True, exist_ok=True)
    return path


def write_log(path, record):
    lines = [
        f"stage: {record.get('stage')}",
        f"timestamp: {datetime.datetime.now().isoformat()}",
        f"commit: {record.get('commit')}",
        f"host_os: {record.get('host_os')}  target: {record.get('target')}",
        f"command: {record.get('command')}",
        f"exit_code: {record.get('exit_code')}",
        f"environment: python={sys.executable}",
        "--- stdout ---",
        record.get("stdout", ""),
        "--- stderr ---",
        record.get("stderr", ""),
        "--- preflight status ---",
        record.get("preflight_status", ""),
        "--- note ---",
        record.get("note", ""),
    ]
    path.write_text("\n".join(lines), encoding="utf-8")


def run_stage(argv, timeout, stage):
    """Run one stage with a timeout. Returns (returncode, stdout, stderr)."""
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        return proc.returncode, proc.stdout or "", proc.stderr or ""
    except subprocess.TimeoutExpired as error:
        note = f"{stage} timed out after {timeout}s: {' '.join(argv)}"
        print(note)
        out = (error.stdout or b"").decode("utf-8", "replace") if isinstance(error.stdout, bytes) else (error.stdout or "")
        err = (error.stderr or b"").decode("utf-8", "replace") if isinstance(error.stderr, bytes) else (error.stderr or "")
        return 124, out, err + "\n" + note
    except OSError as error:
        return 127, "", f"cannot execute {' '.join(argv)}: {error}"


def stage_by_name(plan, name):
    for stage in plan["stages"]:
        if stage["name"] == name:
            return stage
    return None


def convert_game_tree(inspector, game_dir, input_path, out_root, timeout=300):
    """Convert a SELF main executable plus SELF modules beside it.

    Layout is mirrored into ``out_root`` (originals never touched):
      out_root/<input basename>      converted main ELF
      out_root/sce_module/*, prx/*   converted/copied modules
    Returns (extracted_main_path, notes) or (None, error_note).
    """
    out_root = Path(out_root)
    out_root.mkdir(parents=True, exist_ok=True)
    notes = []

    def convert_one(source, dest):
        dest.parent.mkdir(parents=True, exist_ok=True)
        info = inspect_mod.inspect_file(source)
        if info["format"] == "ELF" and info["ok"]:
            dest.write_bytes(Path(source).read_bytes())
            return True, f"copied clean ELF {source} -> {dest}"
        if info["format"] != "SELF":
            return None, f"skipped non-SELF {source} ({info['format']})"
        report = dest.parent / (dest.name + ".self.json")
        code, sout, serr = run_stage(
            [inspector, str(source), "--report", str(report), "--extract", str(dest)],
            timeout, "CONVERT_SELF")
        print(sout)
        if serr:
            print(serr, file=sys.stderr)
        recheck = inspect_mod.inspect_file(dest) if dest.is_file() else None
        if code != 0 or not recheck or not recheck["relinkable"]:
            reason = (serr.strip().splitlines() or ["conversion failed"])[0]
            return False, f"FAILED {source}: {reason}"
        return True, f"converted {source} -> {dest} ({recheck['size']} bytes)"

    main_dest = out_root / Path(input_path).name
    ok, note = convert_one(Path(input_path), main_dest)
    notes.append(note)
    if not ok:
        return None, "; ".join(notes)
    if game_dir:
        root = Path(game_dir)
        for dirname in inspect_mod.KNOWN_MODULE_DIRS:
            source_dir = root / dirname
            if not source_dir.is_dir():
                continue
            for entry in sorted(source_dir.iterdir(), key=lambda p: p.name):
                if not entry.is_file():
                    continue
                status, note = convert_one(entry, out_root / dirname / entry.name)
                notes.append(note)
                if status is False:
                    return None, "; ".join(notes)
    return main_dest, "; ".join(notes)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Prepare (and optionally launch) an AnyPS5 game.")
    parser.add_argument("--target", default=preflight_mod.DEFAULT_TARGET,
                        choices=list(preflight_mod.TARGETS))
    parser.add_argument("--game-dir", default="", help="game directory")
    parser.add_argument("--input", default="", help="input executable override")
    parser.add_argument("--output", default="", help="relink output override")
    parser.add_argument("--relinker", default="", help="Relinker binary override")
    parser.add_argument("--build-dir", default="", help="build directory override")
    parser.add_argument("--run", action="store_true", help="launch the game after successful preparation")
    parser.add_argument("--run-timeout", type=int, default=30, help="game launch timeout in seconds")
    parser.add_argument("--relink-timeout", type=int, default=300, help="Relinker timeout in seconds")
    parser.add_argument("--relink-arg", action="append", default=[], help="extra Relinker arg (repeatable)")
    parser.add_argument("--dry-run", action="store_true", help="stop after PREFLIGHT without relinking")
    args = parser.parse_args(argv)

    log = log_path()
    base = {"commit": preflight_mod.git_info(ROOT).get("commit", "unknown"),
            "target": args.target,
            "host_os": preflight_mod.detect_host()["host_os"]}

    plan, blocked = plan_mod.build_plan(
        target=args.target, game_dir=args.game_dir, input_path=args.input,
        output_path=args.output, relinker_path=args.relinker, build_dir=args.build_dir,
        extra_relink_args=tuple(args.relink_arg), want_run=args.run,
        relink_timeout=args.relink_timeout, run_timeout=args.run_timeout)
    if args.dry_run:
        # Dry run still executes the PREFLIGHT stage for real evidence.
        stage = stage_by_name(plan, STAGE_PREFLIGHT)
        code, out, err = run_stage(stage["command"], stage["timeout"], STAGE_PREFLIGHT)
        print(out)
        write_log(log, {**base, "stage": STAGE_PREFLIGHT, "command": " ".join(stage["command"]),
                        "exit_code": code, "stdout": out, "stderr": err,
                        "preflight_status": plan["preflight_status"],
                        "note": "dry-run: stopped after PREFLIGHT."})
        return code

    # ---- PREFLIGHT (as a real subprocess, exactly as a user would call it)
    stage = stage_by_name(plan, STAGE_PREFLIGHT)
    code, out, err = run_stage(stage["command"], stage["timeout"], STAGE_PREFLIGHT)
    print(out)
    if err:
        print(err, file=sys.stderr)
    if code != 0:
        write_log(log, {**base, "stage": STAGE_PREFLIGHT, "command": " ".join(stage["command"]),
                        "exit_code": code, "stdout": out, "stderr": err,
                        "preflight_status": "blocked (see above)",
                        "note": "Stopped after PREFLIGHT: blocking issue must be fixed first."})
        print(f"[{STAGE_PREFLIGHT}] blocked, log: {log}")
        return 2
    print(f"[{STAGE_PREFLIGHT}] OK")

    if blocked is not None and blocked["stage"] in ("CONVERT_SELF", STAGE_RELINK):
        write_log(log, {**base, "stage": blocked["stage"], "command": "",
                        "exit_code": 2, "stdout": out, "stderr": err,
                        "preflight_status": plan["preflight_status"],
                        "note": f"Plan blocked at {blocked['stage']}: {blocked['reason']}. "
                                f"Evidence: {blocked['evidence']}. Next: {blocked['next']}"})
        print(f"[{blocked['stage']}] blocked: {blocked['reason']}, log: {log}")
        return 2
    if blocked is not None:
        # RUNTIME_START/GAME_RUNNING gates (libs, Vulkan device) do not stop
        # preparation; they are enforced at launch time below.
        print(f"[plan] advisory: {blocked['stage']} blocked: {blocked['reason']}; "
              f"continuing preparation (no --run launch gate yet)")

    relink = stage_by_name(plan, STAGE_RELINK)
    convert = stage_by_name(plan, "CONVERT_SELF")

    # ---- CONVERT_SELF (only via project tool, separate dir, re-verify)
    if convert is not None and convert["command"]:
        inspector = convert["command"][0]
        out_root = (Path(args.game_dir) / "_anyps5_extracted") if args.game_dir \
            else (ROOT / "_anyps5_extracted")
        extracted, note = convert_game_tree(
            inspector, args.game_dir, convert["inputs"].get("input", ""),
            out_root, convert["timeout"])
        print(note)
        if extracted is None:
            write_log(log, {**base, "stage": STAGE_RELINK,
                            "command": " ".join(convert["command"]),
                            "exit_code": 2, "stdout": "", "stderr": note,
                            "preflight_status": plan["preflight_status"],
                            "note": f"SELF tree conversion failed: {note}"})
            print(f"[{STAGE_RELINK}] SELF conversion failed, log: {log}")
            return 2
        # Re-plan the RELINK stage against the converted tree (fresh evidence).
        # The game output stays beside the original game dir, not the
        # converted tree: guest artifacts (app0/) belong to the game layout.
        default_out = str(Path(args.game_dir) / ("app.exe" if args.target == "windows" else "app.elf")) \
            if args.game_dir else ""
        plan2, _ = plan_mod.build_plan(
            target=args.target, game_dir=str(out_root), input_path=str(extracted),
            output_path=args.output or default_out, relinker_path=args.relinker,
            build_dir=args.build_dir, extra_relink_args=tuple(args.relink_arg),
            want_run=args.run, relink_timeout=args.relink_timeout,
            run_timeout=args.run_timeout)
        relink = stage_by_name(plan2, STAGE_RELINK)

    relink_argv = list(relink["command"])
    if not relink_argv:
        write_log(log, {**base, "stage": STAGE_RELINK, "command": "",
                        "exit_code": 2, "stdout": "", "stderr": "no relink command proposed",
                        "preflight_status": plan["preflight_status"],
                        "note": "Preflight passed but proposed no command; refusing to guess."})
        return 2

    # ---- repeat guard: never rerun an identical failed command
    if preflight_mod.is_repeat_command(relink_argv):
        print(f"[{STAGE_RELINK}] refusing to repeat an already-failed identical command: "
              f"{' '.join(relink_argv)} (see logs/.last_failed_command.json)")
        return 2

    # ---- RELINK (exit 0 is not enough: the output artifact is verified)
    started = time.time()
    code, out, err = run_stage(relink_argv, relink["timeout"], STAGE_RELINK)
    print(out)
    if err:
        print(err, file=sys.stderr)
    if code != 0:
        preflight_mod.record_failure(relink_argv, f"relink exit {code} in {time.time() - started:.1f}s")
        write_log(log, {**base, "stage": STAGE_RELINK, "command": " ".join(relink_argv),
                        "exit_code": code, "stdout": out, "stderr": err,
                        "preflight_status": plan["preflight_status"],
                        "note": "Relink failed; command recorded so it is not blindly repeated."})
        print(f"[{STAGE_RELINK}] failed in {time.time() - started:.1f}s, log: {log}")
        return 2
    print(f"[{STAGE_RELINK}] done in {time.time() - started:.1f}s")

    output_file = Path(relink["expected_artifacts"][0])
    check = inspect_mod.inspect_output(output_file, args.target)
    if not check["ok"]:
        write_log(log, {**base, "stage": STAGE_RELINK, "command": " ".join(relink_argv),
                        "exit_code": 2, "stdout": out, "stderr": err,
                        "preflight_status": plan["preflight_status"],
                        "note": f"output verification failed: {check['reason']}"})
        print(f"[{STAGE_RELINK}] output verification failed: {check['reason']}")
        return 2

    if not args.run:
        write_log(log, {**base, "stage": STAGE_RELINK, "command": " ".join(relink_argv),
                        "exit_code": 0, "stdout": out, "stderr": err,
                        "preflight_status": plan["preflight_status"],
                        "note": f"Prepared only (conversion OK, output verified: {check['reason']}). "
                                "This is NOT a game launch. Pass --run to launch."})
        print(f"[{STAGE_RELINK}] OK ({check['reason']}). Not launched (no --run). Log: {log}")
        return 0

    # ---- RUNTIME_START -> GAME_RUNNING (bounded, never infinite)
    runtime = stage_by_name(plan, STAGE_RUNTIME_START)
    missing = [p for p in runtime["preconditions"] if p.startswith("MISSING:")]
    if missing:
        write_log(log, {**base, "stage": STAGE_RUNTIME_START, "command": " ".join(relink_argv),
                        "exit_code": 2, "stdout": out, "stderr": err,
                        "preflight_status": plan["preflight_status"],
                        "note": f"launch preconditions unmet: {'; '.join(missing)}"})
        print(f"[{STAGE_RUNTIME_START}] blocked: {'; '.join(missing)}, log: {log}")
        return 2
    launch = [str(output_file)]
    code, run_out, run_err = run_stage(launch, args.run_timeout, STAGE_RUNTIME_START)
    stage_name = STAGE_GAME_RUNNING if code == 0 else STAGE_RUNTIME_START
    write_log(log, {**base, "stage": stage_name, "command": " ".join(launch),
                    "exit_code": code, "stdout": run_out, "stderr": run_err,
                    "preflight_status": plan["preflight_status"],
                    "note": f"launch timeout {args.run_timeout}s; exit 124 means still running at timeout."})
    print(f"[{stage_name}] exit={code}, log: {log}")
    return code


if __name__ == "__main__":
    sys.exit(main())
