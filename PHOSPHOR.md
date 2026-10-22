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

### Launch Phosphor

MiSTer's OSD list (Core, Define buttons, Video processing, ...) has **Launch Phosphor** under Video
processing, when Phosphor is installed. In the menu it starts the shell (or brings it back); in a
game it loads the menu core and the shell takes the screen. Quitting the shell brings MiSTer's
menu back.

### The menu core

When the Phosphor menu core is installed (`/media/fat/phosphor/cores/menu_phosphor.rbf`), Main loads it
wherever it would load `menu.rbf` (leaving a game, Reboot). It's MiSTer's menu core plus the front ports,
which stay off until Phosphor asks. `menu.rbf` itself is never touched; remove that file and the stock
one loads again.

### Front ports

With the Phosphor menu core ([phosphor-menu](https://github.com/f3bandit/phosphor-menu)) loaded, Main
can read PlayStation controllers and memory cards on the SuperStation One's front ports (SNAC). With
the stock menu core these say `"available":false`.

| Command | |
|---|---|
| `ports` | `{"ok":true,"available":true,"on":false}` |
| `ports on` / `ports off` | The user port goes to the front ports (MT32-pi and MIDI off) or back. It belongs to the connection: when it closes, the ports go off. |
| `pads` | Both ports: connected, controller ID (65 digital, 115 analog), buttons (pressed = 1: bit 0 Select, L3, R3, Start, Up, Right, Down, Left, L2, R2, L1, R1, Triangle, Circle, Cross, bit 15 Square), sticks |
| `pad_analog 1` / `pad_analog 2` | Switch that port's DualShock to analog |
| `card_read P S` | Sector S (0–1023) of the card in port P: `{"ok":true,"data":"<256 hex digits>"}` |
| `card_write P S HEX` | Write 128 bytes to sector S. Errors: `no card`, `bad reply`, `out of range`, `timeout`, `busy` |

One card operation at a time; its reply comes when the card has answered (about 5 ms). Retries and
backups before writing are the caller's job (phosphord).

Apart from the front-port commands (phosphor-menu's commands 0x80–0x85 on hps_io's EXT_BUS, sent
only while the menu core is loaded), nothing talks to the FPGA directly: it all goes through Main's
own functions, from Main's UI loop.

Coming: pause and save-state slots (cores that support them).

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
