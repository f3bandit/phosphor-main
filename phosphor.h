// Phosphor: a small local API so the Phosphor shell and phosphord can work with Main instead of
// around it. Part of phosphor-main (a fork of MiSTer-devel/Main_MiSTer), GPL-3.0 like Main.
#ifndef PHOSPHOR_H
#define PHOSPHOR_H

#include <stdint.h>

// called from the UI loop (scheduler.cpp / main.cpp): accepts clients, runs their commands, sends events
void phosphor_poll(void);
// a controller button (MiSTer's JOY_* mask) while the framebuffer is up: 1 if it went to the shell
int phosphor_pad(uint32_t mask, int press);

// called by HandleUI with each key code it's about to act on; returns the code to use (0: swallowed).
// While Phosphor owns the screen in the menu core, Main's menu ignores keys (the shell has them);
// with the in-game sheet on, a short menu-button press goes to Phosphor and a long one opens the OSD.
uint32_t phosphor_filter_key(uint32_t c);

// "Launch Phosphor" in the OSD: installed (its phosphor.sh is there), and start it or go back to it
// (the shell takes the screen through this API; from a game the menu core loads first); 1 done
int phosphor_installed(void);
// the Phosphor menu core (phosphor-menu) when installed: loaded wherever Main loads menu.rbf; else NULL
const char *phosphor_menu_core(void);
// MiSTer.ini's main= is the default ("MiSTer"): Phosphor's Main stays instead of handing over to it
int phosphor_keep_main(const char *cfg_main);
// while Phosphor's shell is starting (Main just started in the menu core), Main's file browser stays
// closed instead of popping up first; 0 once the shell has the screen, gave it back, or 60 s passed
int phosphor_hold_menu(void);
int phosphor_start(void);

#endif
