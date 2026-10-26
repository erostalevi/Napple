# Developer test drivers (not part of the product)

All of them take the GPU lock (`work/gpu.lock`, `mkdir` to acquire, released on exit) because only one game process may use the GPU at a time.
Build the key injector once: `clang -fobjc-arc -framework Cocoa -framework ApplicationServices -o work/scratch/synth_keys tools/dev/synth_keys.m`
(the terminal app needs the Accessibility permission to post events).

- `dev_gpurun.sh <log> <cmd...>`: run any command under the lock, output to the log.
- `dev_wintest2.sh <name> <datadir> <secs> <viewshot-secs> <png> <synth-delay> <key specs>`: windowed run of the dev binary with a low volume; `OVERLAY_TAB=2` opens the overlay on the Controls tab and renders it to the PNG.
- `dev_apptest.sh <name> <App.app> <secs> <synth-delay> [key specs]`: launches the packaged app through LaunchServices (`open`, the double-click path) and posts real HID key events to the frontmost app;
  env `SCRIPT=` passes `NAPPLE_INPUT_SCRIPT`, `SHOT_FRAMES=` the screenshot frames. Uses `--save-root /tmp/napple-testroot` (outside Documents, which would trigger the macOS folder prompt).
- `dev_usr1test.sh <datadir> <secs> <first-usr1-at> <name> [more gaps]`: headless run with thread dumps (`kill -USR1`).
Key specs for synth_keys: macOS keycodes, `12+14:1.0` presses Q+E together for 1.0 s (L+R in the default layout).
