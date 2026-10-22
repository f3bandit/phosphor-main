# phosphor-main

[Phosphor](https://github.com/f3bandit/phosphor_os)'s fork of [MiSTer Main](https://github.com/MiSTer-devel/Main_MiSTer).
GPL-3.0, like Main. The `master` branch follows MiSTer-devel unchanged; Phosphor's work is on `phosphor`,
rebased onto each MiSTer release.

## What it adds

A small local API so Phosphor works *with* Main instead of around it (`phosphor.cpp`, plus three hooks:
`scheduler.cpp`/`main.cpp` call `phosphor_poll()` in the UI loop, and `menu.cpp` passes keys through
`phosphor_filter_key()` and switches the screen in `HandleUI`). Everything else is Main as it is.

Unix socket `/tmp/phosphor-main.sock` (root only), one command per line, one JSON line back:

| Command | |
|---|---|
| `hello` | `{"ok":true,"api":1,"name":"phosphor-main"}` |
| `status` | core, menu core or not, OSD visible, framebuffer shown, who has the screen, sheet on/off |
| `screen phosphor` / `screen main` | In the menu core, Phosphor's shell (Linux framebuffer) has the screen and Main's menu ignores the controls; or give it back. It belongs to the connection: if the shell stops or crashes, Main takes the screen back by itself. |
| `sheet on` / `sheet off` | In a game: a short menu-button press goes to Phosphor (in-game sheet), a long press (0.7 s) opens the OSD as usual |
| `launch <path>` | Load a core (.rbf) or .mgl/.mra, like `load_core` on `/dev/MiSTer_cmd` (files under /media only) |
| `subscribe` | Also get events: `{"event":"core","name":"SNES","menu":false}`, `{"event":"menu_button"}`, `{"event":"osd","open":true}` |

Nothing talks to the FPGA directly: it all goes through Main's own functions, from Main's UI loop.

Coming: pause and save-state slots (cores that support them), and relaying the Phosphor menu core's
pad and memory-card access ([phosphor-menu](https://github.com/f3bandit/phosphor-menu)).

## Building

`make` with ARM GNU toolchain 10.2-2020.11 (`arm-none-linux-gnueabihf`), the one MiSTer-devel uses
(a newer GCC links against a newer glibc than MiSTer's Linux has). GitHub Actions ("Build phosphor-main")
does this and keeps `bin/MiSTer` as an artifact. `phosphor_tests/run.sh` tests the API on a PC.

## Trying it on a MiSTer

Nothing installs it yet. To try it, stop Main and start this one by hand (it's undone by a reboot):

```
killall MiSTer; sleep 1; /media/fat/phosphor/MiSTer_phosphor &
```

Then start the Phosphor shell: it takes the screen through the API (no F9).
