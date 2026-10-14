#!/usr/bin/env bash
# AnyPS5 prepare-and-launch wrapper for Linux (bash).
# Forwards all arguments to tools/run_game.py which owns the workflow:
# PREFLIGHT -> RELINK -> RUNTIME_START -> GAME_RUNNING.
# Example: ./tools/run_game.sh --game-dir "$HOME/games/MyGame" --target linux
set -euo pipefail
exec python3 "$(dirname "$0")/run_game.py" "$@"
