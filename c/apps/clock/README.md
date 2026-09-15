# Clock application

This directory owns the application, not its rendering implementation.

| File | Responsibility |
| --- | --- |
| `main.c` | Window lifecycle, event dispatch, wall-time sampling, agent context |
| `model.c`, `model.h` | Clock display preferences and semantic actions |
| `../gui/clock/` | Layout, dial, controls, cached surfaces and motion |

The GUI reads `const struct clock_state` and returns `enum clock_action`.
Only the application applies the action. Keyboard H and the format button
therefore use the same state transition; S and the seconds button do too.
Preferences currently last for this process; they are not saved to system
settings. Wall time comes from `get_time`, without a claimed timezone database.

The installed app remains `/clock.aex`, titled Clock, with ID `os.logit.clock`.
The catalog points to `main.c`. `tests/clock_ui.mk` links both application and GUI
translation units; `Makefile` includes it at the former single-file Clock rule.
No example program or SDK source copy is embedded into the application.

UI: a 620x400 default window presents an analog dial beside a digital readout,
seconds, time format and date. Below 520 points it stacks vertically; very small
windows omit the dial to preserve the readout. H switches 12/24-hour time; S
shows/hides seconds. Buttons also support normal mouse and AUI keyboard focus.
Second-hand ticks ease for 180 ms via the SDK; reduced motion shows exact ticks.
Minimized windows do not draw; restoring reads the current wall time.

Validation commands, with an explicit isolated `BUILD`:

```sh
make BUILD=/path/to/isolated-build test-clock-ui test-mk-wired
make BUILD=/path/to/isolated-build /path/to/isolated-build/logit.iso /path/to/isolated-build/disk.img
make BUILD=/path/to/isolated-build test-clock-ui-os
```

2026-09-15 evidence is in `/Users/wangzhe/system/openlogit-clock-0915`:
full ISO/disk build, 11 host assertions under ASan/UBSan, the failing no-motion
control, and 12 guest UI checks at each of 100% and 150%. Guest tests launch the
installed app normally and check screenshots, controls, resize, animation,
minimize/restore and reduced motion. Frame timings bracket drawing through the
present request; they are not input-to-photon latency or vblank/FPS measurements.
