// Copyright (C) 2026 f3bandit. GPL-3.0 (see LICENSE).
// A stand-in Main for working on Phosphor on a PC: phosphor.cpp with the stubs (stubs.cpp) behind
// it, serving the real socket API. FAKE_EXT=0 starts with the stock menu core; "launch" of a
// file named menu_phosphor.rbf restarts it with the Phosphor one, like Main restarting on a core load.
//   g++ -I.. fake_main.cpp stubs.cpp ../phosphor.cpp -o fake_main && PHOSPHOR_MAIN_SOCK=/tmp/m.sock ./fake_main
// Copyright (C) 2026 f3bandit. GPL-3.0 (see LICENSE).
#include "phosphor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
extern int g_ext, g_card_present; extern char g_loaded[512]; extern uint8_t g_card[];
int main(int argc, char **argv)
{
	(void)argc;
	const char *e = getenv("FAKE_EXT");
	g_ext = !e || strcmp(e, "0");
	g_card_present = !getenv("FAKE_NO_CARD");
	for (int i = 0; i < 1024 * 128; i++) g_card[i] = (uint8_t)(i * 31 + (i >> 7));
	g_card[0] = 'M'; g_card[1] = 'C';
	setvbuf(stdout, NULL, _IONBF, 0);
	for (;;)
	{
		phosphor_poll();
		if (strstr(g_loaded, "menu_phosphor.rbf"))
		{
			setenv("FAKE_EXT", "1", 1);
			execv("/proc/self/exe", argv);
		}
		usleep(1000);
	}
}
