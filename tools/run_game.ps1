# AnyPS5 prepare-and-launch wrapper for Windows (PowerShell).
# Forwards all arguments to tools/run_game.py which owns the workflow:
# PREFLIGHT -> RELINK -> RUNTIME_START -> GAME_RUNNING.
# Example: .\tools\run_game.ps1 --game-dir "D:\Games\MyGame" --target windows
$ErrorActionPreference = 'Stop'
$script = Join-Path $PSScriptRoot 'run_game.py'
& python $script @args
exit $LASTEXITCODE
