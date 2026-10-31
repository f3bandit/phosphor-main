// Phosphor: game ports (Quake, Duke Nukem 3D): games that run on the ARM side while their core only puts
// the picture out. Each used to need its own Main (MiSTer_Quake, Mister_duke3d: older Main_MiSTer forks
// with a wrapper); Phosphor's Main does the wrapper's job itself, so the game gets today's Main (no HDMI
// CEC errors before it starts), Phosphor's in-game menu and a way back to Phosphor.
// Part of phosphor-main (a fork of MiSTer-devel/Main_MiSTer), GPL-3.0 like Main.
// Based on the MiSTer_Quake and MiSTer_Duke3D wrappers by neofreno (GPL-2.0-or-later), which follow
// kimchiman52's sonic-mania-mister wrapper.
// Copyright (C) 2026 f3bandit. GPL-3.0 (see LICENSE).
#ifndef GAMEPORT_H
#define GAMEPORT_H

// main= values for the game ports' own Mains: Phosphor's Main stays for these (phosphor_keep_main)
int gameport_main_merged(const char *main_name);

// after user_io_init: the loaded core is a game port? Then check its data and start it (1); 0: not a port
int gameport_begin(const char *rbf_path);

// from the UI loop: controllers to the game, the game started after the core settles, restarts, its end
void gameport_poll(void);

// before Main restarts or loads another core: the game stopped, the ARM clock and files put back
void gameport_stop(void);

int gameport_running(void);

#endif
