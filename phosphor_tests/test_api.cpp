// Tests for phosphor.cpp (Phosphor API in Main). GPL-3.0 like the rest of this repo.
#include "phosphor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#define UPSTROKE 0x400000
#define KEY_F12 88
#define KEY_UP 103
extern int g_menu, g_osd, g_fb, g_screen_req; extern char g_core[64], g_loaded[512];
static int conn() { int s = socket(AF_UNIX, SOCK_STREAM, 0); struct sockaddr_un a = {}; a.sun_family = AF_UNIX; strcpy(a.sun_path, getenv("PHOSPHOR_MAIN_SOCK")); assert(!connect(s, (struct sockaddr *)&a, sizeof a)); fcntl(s, F_SETFL, O_NONBLOCK); return s; }
static void say(int s, const char *l) { char b[600]; snprintf(b, sizeof b, "%s\n", l); write(s, b, strlen(b)); }
static char rb[4096];
static const char *got(int s) { for (int i = 0; i < 5; i++) phosphor_poll(); int n = read(s, rb, sizeof rb - 1); rb[n > 0 ? n : 0] = 0; return rb; }
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); exit(1); } else printf("ok   %s\n", m); } while (0)
int main() {
	phosphor_poll();
	int a = conn(), b = conn();
	say(a, "hello"); CHECK(strstr(got(a), "\"api\":1"), "hello");
	say(a, "status"); CHECK(strstr(got(a), "\"owner\":\"main\""), "status: Main owns the screen");
	say(a, "screen phosphor"); CHECK(strstr(got(a), "\"ok\":true") && g_screen_req == 1, "screen phosphor -> framebuffer on");
	say(b, "screen phosphor"); CHECK(strstr(got(b), "another client"), "second client can't take it");
	say(b, "subscribe"); got(b);
	CHECK(phosphor_filter_key(KEY_UP) == 0, "menu core: keys swallowed while Phosphor owns the screen");
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
	say(b, "screen phosphor"); CHECK(strstr(got(b), "\"ok\":true"), "another client can take it now");
	char big[3000]; memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = 0; write(b, big, strlen(big)); got(b);
	int c = conn(); say(c, "status"); const char *st = got(c); printf("status after long line: %s\n", st); CHECK(strstr(st, "\"owner\":\"main\""), "line too long -> client dropped, screen back");
	say(c, "bogus"); CHECK(strstr(got(c), "unknown command"), "unknown command");
	printf("ALL PASSED\n");
}
