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

## Physical discs

Games from the dock's CD/DVD drive (`/dev/sr0`), merged from
[Main_MiSTer_Physical_Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc) by **Anime0t4ku** and **bonna79**
(GPL-3.0; merged at their commit e6e62b2, 2026-10-14, after v2.10.0). Their work, unchanged: PlayStation (with
LibCrypt from the disc's subchannel), Saturn, Mega-CD (CD+G), PC Engine CD, Neo Geo CD, CD-i and 3DO (experimental),
SNES MSU-1 and MD+ from a disc, CD audio and disc swapping. Their code is in `support/physical_disc/` and the
per-system files; their notes and options are in their README.

What's different in Phosphor's Main:
- **One Main.** Upstream is a separate Main that MiSTer switches to through `[A0CD-*] main=MiSTer_Physical-CD`
  sections in MiSTer.ini. Here it's built into Phosphor's Main, so Phosphor's API (screen, controllers, front
  ports, mapping) keeps working while a disc game runs. No `main=` sections are needed.
- **Auto Disc Discovery is off by default** (`cfg.cpp`): Phosphor's Main is always the menu's Main and Phosphor
  owns the screen, so a disc going in mustn't start a core by itself. Phosphor decides (its disc screen and
  setting). `PHYSICAL_DISC_MGL=1` in MiSTer.ini still turns upstream's behaviour on.
- Disc games are started the upstream way, with `.mgl` files that load the stock cores under an `A0CD-*` set
  name (the name tells the disc code the core was started for a disc). CD-i uses its own core fork.
- Merged with `-X ignore-space-change` and blank-line padding taken out, so MiSTer's own lines stay as MiSTer
  has them and later MiSTer releases merge cleanly. Conflicts were with newer MiSTer code (Saturn RAM carts,
  CD-i reads, the file browser): MiSTer's version kept, their disc branches added to it.

Updating from upstream: `git fetch a0cd && git merge -X ignore-space-change a0cd/master` on the `disc-support`
branch, then build and run `phosphor_tests/run.sh`.

## Building

`make` with ARM GNU toolchain 10.2-2020.11 (`arm-none-linux-gnueabihf`), the one MiSTer-devel uses
(a newer GCC links against a newer glibc than MiSTer's Linux has). GitHub Actions ("Build phosphor-main")
does this and keeps `bin/MiSTer` as an artifact. `phosphor_tests/run.sh` tests the API on a PC.

## Running it

Phosphor's SD card image boots with it: MiSTer's own Main starts as usual and is swapped for this one
(`phosphor.sh boot`, from Phosphor's block in `linux/user-startup.sh`); `/media/fat/MiSTer` itself is
never replaced, and it's never started while another Main (Console Mode's, for one) is running.

On a card with Phosphor installed, by hand (undone by a reboot):

```
/media/fat/phosphor/phosphor.sh main     # this Main in place of MiSTer's
/media/fat/phosphor/phosphor.sh start    # the shell; it takes the screen through the API
```

## Licence

Phosphor's Main is a fork of MiSTer's Main ([MiSTer-devel/Main_MiSTer](https://github.com/MiSTer-devel/Main_MiSTer))
with odelot's RetroAchievements Main ([odelot/Main_MiSTer](https://github.com/odelot/Main_MiSTer)) merged in,
all under the GNU General Public License version 3 (LICENSE). Phosphor's own files (phosphor*.cpp/h,
phosphor_tests/) are Copyright (C) 2026 f3bandit under the same licence. rcheevos (lib/rcheevos) is MIT
(lib/rcheevos/LICENSE).

`achievement.wav`, the unlock sound, comes from odelot's RetroAchievements Main, where it was added with
his work; its origin and licence are his, and it is not Phosphor's. Credit: odelot. If it is yours and you
want it credited differently or taken out, open an issue and it will be.
