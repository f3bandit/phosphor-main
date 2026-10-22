// Phosphor: a small local API so the Phosphor shell and phosphord can work with Main instead of
// around it. Part of phosphor-main (a fork of MiSTer-devel/Main_MiSTer), GPL-3.0 like Main.
#ifndef PHOSPHOR_H
#define PHOSPHOR_H

#include <stdint.h>

// called from the UI loop (scheduler.cpp / main.cpp): accepts clients, runs their commands, sends events
void phosphor_poll(void);

// called by HandleUI with each key code it's about to act on; returns the code to use (0: swallowed).
// While Phosphor owns the screen in the menu core, Main's menu ignores keys (the shell has them);
// with the in-game sheet on, a short menu-button press goes to Phosphor and a long one opens the OSD.
uint32_t phosphor_filter_key(uint32_t c);

// System Settings > Phosphor: installed (its phosphor.sh is there), and start it (the shell then
// takes the screen through this API); 1 started
int phosphor_installed(void);
int phosphor_start(void);

#endif
