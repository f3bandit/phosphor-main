// Tests for phosphor.cpp (Phosphor API in Main). GPL-3.0 like the rest of this repo.
// Copyright (C) 2026 f3bandit. GPL-3.0 (see LICENSE).
#include "phosphor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <stdint.h>
#define UPSTROKE 0x400000
#define KEY_F12 88
#define KEY_UP 103
extern int g_shots; extern char joy_bnames[32][32]; extern int joy_bcount;
extern int g_menu, g_osd, g_fb, g_screen_req; extern unsigned int g_menu_key; extern char g_core[64], g_loaded[512];
extern int g_vt, g_fbcon; extern int g_ext, g_card_present, g_card_busy_polls, g_pad_cmds[8], g_pad_ncmd; extern uint16_t g_ext_mode; extern uint8_t g_card[];
static int conn() { int s = socket(AF_UNIX, SOCK_STREAM, 0); struct sockaddr_un a = {}; a.sun_family = AF_UNIX; strcpy(a.sun_path, getenv("PHOSPHOR_MAIN_SOCK")); assert(!connect(s, (struct sockaddr *)&a, sizeof a)); fcntl(s, F_SETFL, O_NONBLOCK); return s; }
static void say(int s, const char *l) { char b[600]; snprintf(b, sizeof b, "%s\n", l); write(s, b, strlen(b)); }
static char rb[8192];
static const char *got(int s) { for (int i = 0; i < 5; i++) phosphor_poll(); int n = read(s, rb, sizeof rb - 1); rb[n > 0 ? n : 0] = 0; return rb; }
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); exit(1); } else printf("ok   %s\n", m); } while (0)
int main() {
	phosphor_poll();
	int a = conn(), b = conn();
	say(a, "hello"); CHECK(strstr(got(a), "\"api\":6") && strstr(rb, "\"pads\":true") && strstr(rb, "\"scripts\":true") && strstr(rb, "\"mapping\":true") && strstr(rb, "\"disc\":true") && strstr(rb, "\"ingame\":true") && strstr(rb, "\"ra\":true"), "hello");
	say(a, "ra"); CHECK(strstr(got(a), "\"active\":false"), "ra: no RetroAchievements core, inactive");
	say(a, "ra list"); CHECK(strstr(got(a), "\"achievements\":[]"), "ra list: empty without a game");
	say(a, "status"); CHECK(strstr(got(a), "\"owner\":\"main\""), "status: Main owns the screen");
	CHECK(strstr(rb, "\"file\":\"\""), "status: no file loaded yet");
	phosphor_file_loaded("/media/usb0/games/NES/Castle \"Quest\".nes");
	say(a, "status"); CHECK(strstr(got(a), "\"file\":\"/media/usb0/games/NES/Castle \\\"Quest\\\".nes\""), "status: the file loaded, quoted for JSON");
	say(a, "screen phosphor"); CHECK(strstr(got(a), "\"ok\":true") && g_screen_req == 1, "screen phosphor -> framebuffer on");
	say(b, "screen phosphor"); CHECK(strstr(got(b), "another client"), "second client can't take it");
	say(b, "subscribe"); got(b);
	CHECK(phosphor_filter_key(KEY_UP) == 0, "menu core: keys swallowed while Phosphor owns the screen");
	// controllers: Main's buttons go to the shell, held directions repeat
	CHECK(phosphor_pad(0x08, 1) == 1 && strstr(got(a), "\"event\":\"pad\",\"b\":\"up\",\"p\":1"), "pad: up pressed -> event to the shell");
	usleep(500000); CHECK(strstr(got(a), "\"b\":\"up\",\"p\":2"), "pad: held up repeats");
	CHECK(phosphor_pad(0x08, 0) == 1 && strstr(got(a), "\"b\":\"up\",\"p\":0"), "pad: up released");
	usleep(400000); CHECK(!strstr(got(a), "\"p\":2"), "pad: no repeat after release");
	CHECK(phosphor_pad(0x100, 1) == 1 && strstr(got(a), "\"b\":\"x\""), "pad: X goes too (MiSTer's terminal keys don't have it)");
	phosphor_pad(0x100, 0); got(a);
	// scripts, the way MiSTer's Scripts menu runs them
	say(b, "script /media/phtest/t.sh"); CHECK(strstr(got(b), "take the screen first"), "script: only the screen's owner");
	say(a, "script /etc/passwd"); CHECK(strstr(got(a), "no such script"), "script: only under /media");
	say(a, "script /media/phtest/../t.sh"); CHECK(strstr(got(a), "no such script"), "script: no ..");
	say(a, "script /media/phtest/t.sh"); CHECK(strstr(got(a), "\"ok\":true") && g_vt == 2 && g_fbcon == 1, "script: MiSTer's terminal on tty2");
	say(a, "script /media/phtest/t.sh"); CHECK(strstr(got(a), "a script is running"), "script: one at a time");
	CHECK(phosphor_pad(0x10, 1) == 0, "script running: buttons are the terminal's keys");
	usleep(200000); got(a);                                    // (agetty isn't here: the child ends at once)
	CHECK(phosphor_filter_key(KEY_UP) == 0, "script: keys aren't Main's menu's");
	{ int r = phosphor_pad(0x10, 1); const char *e = got(a); CHECK(r == 1 && strstr(e, "\"event\":\"script_done\",\"status\":") && g_vt == 1, "press any key -> done event, tty1 again"); }
	phosphor_pad(0x10, 0); got(a);
	CHECK(phosphor_filter_key(KEY_F12) == 0 && phosphor_filter_key(KEY_F12 | UPSTROKE) == 0, "menu core: menu button swallowed");
	CHECK(strstr(got(b), "menu_button"), "menu button event sent");
	g_fb = 0; got(b); CHECK(g_fb == 1, "framebuffer turned back on if Main switched it off");
	// a game
	g_menu = 0; strcpy(g_core, "SNES"); CHECK(strstr(got(b), "\"event\":\"core\",\"name\":\"SNES\""), "core event");
	CHECK(phosphor_filter_key(KEY_UP) == KEY_UP, "in a game: keys pass");
	CHECK(phosphor_filter_key(KEY_F12) == KEY_F12, "in a game, sheet off: menu button opens the OSD");
	say(a, "sheet on"); got(a);
	CHECK(phosphor_filter_key(KEY_F12) == 0, "sheet on: press held back");
	CHECK(phosphor_filter_key(KEY_F12 | UPSTROKE) == 0 && strstr(got(b), "menu_button"), "short press -> sheet event");
	phosphor_filter_key(KEY_F12); usleep(800000); phosphor_filter_key(KEY_F12);
	CHECK(phosphor_filter_key(KEY_F12 | UPSTROKE) == (KEY_F12 | UPSTROKE), "long press -> OSD");
	g_osd = 1; CHECK(strstr(got(b), "\"osd\",\"open\":true"), "osd event");
	g_osd = 0; g_menu = 1; strcpy(g_core, "MENU"); g_fb = 0; g_screen_req = -9; got(b);
	CHECK(g_screen_req == 1, "back in the menu: Phosphor's screen again");
	// launch
	say(a, "launch /etc/passwd"); CHECK(strstr(got(a), "no such file"), "launch outside /media refused");
	say(a, "launch /media/../etc/passwd"); CHECK(strstr(got(a), "no such file"), "launch with .. refused");
	// shell crashes: Main takes the screen back
	close(a); g_screen_req = -9; got(b);
	CHECK(g_screen_req == 0, "owner gone -> screen back to Main");
	CHECK(phosphor_filter_key(KEY_F12) == KEY_F12, "and Main's menu works again");
	CHECK(phosphor_start() == 1 && strstr(got(b), "\"event\":\"show\""), "Launch Phosphor while the shell gave the screen back -> show event");
	{
		// a client that dies before Main notices EOF: the failed send must give back the screen too
		int z = conn(); say(z, "screen phosphor"); got(z); say(z, "subscribe"); got(z);
		CHECK(phosphor_filter_key(KEY_UP) == 0, "dying client owns the screen");
		close(z);
		phosphor_filter_key(KEY_F12); phosphor_filter_key(KEY_F12 | UPSTROKE);   // event to a closed socket
		for (int i = 0; i < 5; i++) phosphor_poll();
		CHECK(phosphor_filter_key(KEY_UP) == KEY_UP, "send failure -> screen back to Main");
	}
	say(b, "screen phosphor"); CHECK(strstr(got(b), "\"ok\":true"), "another client can take it now");
	char big[3000]; memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = 0; write(b, big, strlen(big)); got(b);
	int c = conn(); say(c, "status"); const char *st = got(c); printf("status after long line: %s\n", st); CHECK(strstr(st, "\"owner\":\"main\""), "line too long -> client dropped, screen back");
	say(c, "bogus"); CHECK(strstr(got(c), "unknown command"), "unknown command");
	// the front ports
	{
		int d = conn(), e = conn();
		say(d, "ports"); CHECK(strstr(got(d), "\"available\":true,\"on\":false"), "ports: available, off");
		say(d, "card_read 1 0"); CHECK(strstr(got(d), "front ports are off"), "card read refused while off");
		say(d, "ports on"); CHECK(strstr(got(d), "\"ok\":true") && g_ext_mode == 1, "ports on");
		say(e, "ports on"); CHECK(strstr(got(e), "another client"), "second client can't take the ports");
		say(d, "pads"); const char *p = got(d); printf("%s", p);
		CHECK(strstr(p, "{\"port\":1,\"connected\":true,\"id\":115,\"buttons\":16,\"rx\":128,\"ry\":128,\"lx\":129,\"ly\":127}"), "pads: port 1 analog, Up held");
		CHECK(strstr(p, "{\"port\":2,\"connected\":false"), "pads: nothing in port 2");
		for (int i = 0; i < 128; i++) g_card[5 * 128 + i] = (uint8_t)(i * 7);
		g_card_busy_polls = 3;
		say(d, "card_read 1 5"); usleep(5000); const char *r = got(d);
		for (int t = 0; t < 50 && !*r; t++) { usleep(3000); r = got(d); }
		CHECK(!strncmp(r, "{\"ok\":true,\"data\":\"00070e15", 27) && strlen(r) == 19 + 256 + 3, "card_read: data (after busy)");
		say(d, "card_read 1 5"); say(d, "card_read 1 6"); r = got(d); CHECK(strstr(r, "busy"), "one card operation at a time");
		for (int t = 0; t < 50 && !strstr(r, "\"data\""); t++) { usleep(3000); r = got(d); }
		char line[400] = "card_write 1 9 ";
		for (int i = 0; i < 128; i++) sprintf(line + strlen(line), "%02x", 255 - i);
		say(d, line); usleep(6000); r = got(d);
		for (int t = 0; t < 50 && !*r; t++) { usleep(3000); r = got(d); }
		CHECK(strstr(r, "\"ok\":true") && g_card[9 * 128] == 255 && g_card[9 * 128 + 127] == 128, "card_write: on the card");
		say(d, "card_write 1 9 zz"); CHECK(strstr(got(d), "256 hex"), "card_write: bad data refused");
		say(d, "card_read 3 0"); CHECK(strstr(got(d), "port is 1 or 2"), "bad port refused");
		say(d, "card_read 1 1024"); CHECK(strstr(got(d), "0..1023"), "bad sector refused");
		say(d, "card_read 2 0"); usleep(6000); r = got(d);
		for (int t = 0; t < 50 && !*r; t++) { usleep(3000); r = got(d); }
		CHECK(strstr(r, "no card"), "no card in port 2");
		say(d, "pad_analog 2"); got(d);
		for (int t = 0; t < 20 && g_pad_ncmd < 3; t++) { usleep(25000); got(d); }
		CHECK(g_pad_ncmd == 3 && g_pad_cmds[0] == 0x101 && g_pad_cmds[1] == 0x102 && g_pad_cmds[2] == 0x103, "pad_analog: config, analog, leave");
		g_ext_mode = 0; say(d, "pads"); got(d); CHECK(g_ext_mode == 1, "menu core reloaded: ports back on");
		close(d); got(e); CHECK(g_ext_mode == 0, "owner gone -> ports off");
		g_ext = 0; strcpy(g_core, "MENU2"); got(e); strcpy(g_core, "MENU"); got(e);
		say(e, "ports"); CHECK(strstr(got(e), "\"available\":false"), "stock menu core: not available");
		say(e, "ports on"); CHECK(strstr(got(e), "not available"), "stock menu core: ports on refused");
		g_ext = 1;
		close(e);
	}
	{
		// the disc and Phosphor's in-game screen
		int f = conn();
		say(f, "disc"); CHECK(strstr(got(f), "\"state\":\"empty\""), "disc: state");
		say(f, "discx"); CHECK(!strstr(got(f), "\"state\""), "disc: other words aren't disc commands");
		g_menu = 1; say(f, "ingame on"); CHECK(strstr(got(f), "no game is running"), "ingame: not in the menu core");
		g_menu = 0; strcpy(g_core, "SNES"); got(f); g_fb = 0;
		say(f, "ingame on"); CHECK(strstr(got(f), "\"ok\":true") && g_fb == 1, "ingame on: Phosphor's screen over the game");
		CHECK(phosphor_pad(0x08, 1) == 1 && strstr(got(f), "\"event\":\"pad\""), "ingame: the controller goes to Phosphor");
		phosphor_pad(0x08, 0); got(f);
		// a direction held while the in-game menu closes: no repeats after, and its release goes to the game
		phosphor_pad(0x08, 1); got(f);
		say(f, "ingame off"); got(f); usleep(400000);
		CHECK(!strstr(got(f), "\"event\":\"pad\""), "ingame off with up held: no repeats to the shell");
		CHECK(phosphor_pad(0x08, 0) == 0 && !strstr(got(f), "\"event\":\"pad\""), "ingame off: the held direction's release goes to the game");
		say(f, "ingame on"); got(f);
		say(f, "ingame off"); CHECK(strstr(got(f), "\"ok\":true") && g_fb == 0, "ingame off: the game again");
		CHECK(phosphor_pad(0x08, 1) == 0, "ingame off: the controller goes to the game");
		phosphor_pad(0x08, 0);
		say(f, "sheet on"); got(f);
		say(f, "ingame on"); got(f);
		say(f, "ingame osd"); CHECK(strstr(got(f), "\"ok\":true"), "ingame osd");
		CHECK(g_menu_key == KEY_F12, "ingame osd: Main presses its menu key");
		CHECK(phosphor_filter_key(KEY_F12) == KEY_F12, "ingame osd: that press goes through (not held for a short press)");
		usleep(100000); phosphor_poll();
		CHECK(g_menu_key == (KEY_F12 | UPSTROKE), "ingame osd: and releases it a moment later (the OSD opens on the release)");
		CHECK(phosphor_filter_key(KEY_F12 | UPSTROKE) == (KEY_F12 | UPSTROKE), "ingame osd: the release goes through: the OSD opens");
		CHECK(phosphor_filter_key(KEY_F12) == 0, "and the next menu button is a short press for the sheet again");
		phosphor_filter_key(KEY_F12 | UPSTROKE); got(f);
		say(f, "sheet off"); got(f);
		phosphor_pad(0x08, 0);
		say(f, "ingame on"); got(f); g_fb = 0; usleep(1000);
		CHECK(strstr(got(f), "\"event\":\"ingame\",\"on\":false"), "Main took the screen back: told");
		close(f); got(f);
		g_menu = 1; strcpy(g_core, "MENU");
	}
	/* screenshots: both sticks in (L3 + R3) */
	{
		int sv = g_menu; g_menu = 0;
		int s0 = g_shots;
		phosphor_thumbs(3, 0, 1);
		CHECK(g_shots == s0, "one stick in: nothing");
		phosphor_thumbs(3, 1, 1);
		CHECK(g_shots == s0 + 1, "both sticks in, in a game: a screenshot");
		phosphor_thumbs(3, 1, 0); phosphor_thumbs(3, 1, 1);
		CHECK(g_shots == s0 + 2, "again: another");
		phosphor_thumbs(3, 0, 0); phosphor_thumbs(3, 1, 0);
		phosphor_thumbs(4, 0, 1); phosphor_thumbs(5, 1, 1);
		CHECK(g_shots == s0 + 2, "two controllers, one stick each: nothing");
		phosphor_thumbs(4, 0, 0); phosphor_thumbs(5, 1, 0);
		{
			int ow = conn(); say(ow, "screen phosphor"); got(ow); int fbw = g_fb; g_fb = 1;     /* Phosphor's in-game menu up */
			phosphor_thumbs(6, 0, 1); phosphor_thumbs(6, 1, 1);
			CHECK(g_shots == s0 + 2, "Phosphor's picture up in a game: not MiSTer's (the shell saves it)");
			phosphor_thumbs(6, 0, 0); phosphor_thumbs(6, 1, 0);
			g_fb = fbw; say(ow, "screen main"); got(ow); close(ow);
		}
		phosphor_thumbs(4, 0, 0); phosphor_thumbs(5, 1, 0);
		int c = conn();
		say(c, "shot off"); got(c);
		phosphor_thumbs(3, 0, 1); phosphor_thumbs(3, 1, 1);
		CHECK(g_shots == s0 + 2, "shot off: the combo does nothing");
		phosphor_thumbs(3, 0, 0); phosphor_thumbs(3, 1, 0);
		say(c, "shot on"); got(c);
		say(c, "screenshot"); CHECK(strstr(got(c), "\"ok\":true") && g_shots == s0 + 3, "screenshot in a game");
		phosphor_screenshot_result(NULL, 1);
		g_menu = 1;
		say(c, "screenshot"); CHECK(strstr(got(c), "no game"), "screenshot in the menu: no");
		g_menu = sv;
	}
	printf("ALL PASSED\n");
}
