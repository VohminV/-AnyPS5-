# Controller input (gamepad-only)

AnyPS5 is driven exclusively by SDL-mapped game controllers. Keyboard and mouse
pad bindings, `anyps5-input.ini`, `ANYPS5_INPUT_CONFIG`, and the F1/F11 help /
fullscreen keys were removed. If an `anyps5-input.ini` file is present beside
the executable it is ignored.

Xbox-style controllers and PlayStation controllers recognized by SDL use the
standard PS button layout; sticks and analog triggers are passed through, and
controllers can be connected or disconnected while the game is running. The
first recognized controller is used.

Xbox mapping (verified against the SDL_GameController standard):

- A (south) -> Cross, B (east) -> Circle, X (west) -> Square, Y (north) -> Triangle
- LB/RB -> L1/R1, LT/RT -> analog L2/R2
- Menu/Start -> Options, View/Back -> TouchPad tap emulation (Xbox has no touchpad)
- D-pad, L3/R3 stick clicks map 1:1; left/right sticks and triggers are analog
- Guide / Share (MISC1) / Elite paddles are ignored

On a PlayStation controller, the touchpad click button and finger coordinates
are forwarded; on Xbox, View emulates a center touch at (960, 471).

The window opens in fullscreen (`SDL_WINDOW_FULLSCREEN_DESKTOP`) immediately
on startup. The window title shows only `Title | FPS: xx.xx (frames)` — no
keyboard hints, no F1 help line, no bindings dialog.
