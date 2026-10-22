// Phosphor: a small local API so the Phosphor shell and phosphord can work with Main instead of
// around it. Part of phosphor-main (a fork of MiSTer-devel/Main_MiSTer), GPL-3.0 like Main.
//
// Unix socket /tmp/phosphor-main.sock (root only), one command per line, one JSON reply per line:
//   hello                  {"ok":true,"api":1,"name":"phosphor-main"}
//   status                 core, menu core or not, OSD visible, framebuffer shown, who owns the screen
//   screen phosphor|main   Phosphor takes the screen in the menu core (Linux framebuffer shown, Main's
//                          menu ignores keys) or gives it back. Ownership belongs to this connection:
//                          if it closes (the shell quit or crashed), Main takes the screen back.
//   sheet on|off           in a game, a short menu-button press goes to Phosphor (event), a long one
//                          (0.7 s) opens the OSD as usual; off: the OSD as usual
//   launch <path>          load a core (.rbf) or a .mgl/.mra, like MiSTer_cmd load_core (under /media only)
//   subscribe              this connection also gets events:
//                          {"event":"core","name":"SNES"} {"event":"menu_button"} {"event":"osd","open":true}
// Nothing here talks to the FPGA directly: it uses Main's own functions, from Main's UI loop.
#include "phosphor.h"
#include "menu.h"
#include "user_io.h"
#include "video.h"
#include "fpga_io.h"
#include "file_io.h"
#include "input.h"
#include "hardware.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "support/arcade/mra_loader.h"          // xml_load: .mra / .mgl
void menu_phosphor_screen(int phosphor);        // menu.cpp

#define PH_SOCK "/tmp/phosphor-main.sock"
#define PH_MAX 4
#define PH_LINE 1024
#define PH_LONG_PRESS_MS 700

typedef struct {
	int fd;
	int subscribed;
	size_t len;
	char buf[PH_LINE];
} ph_client_t;

static int lfd = -1;
static ph_client_t cl[PH_MAX];
static int owner = -1;           // index of the client that owns the screen, -1: Main
static int sheet_client = -1;    // the client that turned the in-game sheet on
static char last_core[64];
static int last_osd = -1;
static unsigned long f12_down;   // when the menu button went down (sheet), 0: not held
static int inited;

static const char *sock_path(void)
{
	const char *p = getenv("PHOSPHOR_MAIN_SOCK");
	return p && *p ? p : PH_SOCK;
}

static void ph_init(void)
{
	inited = 1;
	for (int i = 0; i < PH_MAX; i++) cl[i].fd = -1;
	unlink(sock_path());
	lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (lfd < 0) return;
	struct sockaddr_un a = {};
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock_path());
	mode_t old = umask(077);       // root only
	int ok = bind(lfd, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(lfd, 4) == 0;
	umask(old);
	if (!ok) { printf("phosphor: can't listen on %s\n", sock_path()); close(lfd); lfd = -1; return; }
	printf("phosphor: API on %s\n", sock_path());
}

static void send_line(int i, const char *s)
{
	if (cl[i].fd < 0) return;
	size_t n = strlen(s);
	// small replies on a local socket: if it can't take a whole line now, the client isn't reading
	if (send(cl[i].fd, s, n, MSG_NOSIGNAL | MSG_DONTWAIT) != (ssize_t)n || send(cl[i].fd, "\n", 1, MSG_NOSIGNAL | MSG_DONTWAIT) != 1)
	{
		printf("phosphor: client %d not reading, dropped\n", i);
		close(cl[i].fd);
		cl[i].fd = -1;
	}
}

static void json_str(char *out, size_t n, const char *s)   // s as a JSON string body (no quotes)
{
	size_t j = 0;
	for (; s && *s && j + 7 < n; s++)
	{
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') { out[j++] = '\\'; out[j++] = (char)c; }
		else if (c < 0x20) j += snprintf(out + j, n - j, "\\u%04x", c);
		else out[j++] = (char)c;
	}
	out[j] = 0;
}

static void event_all(const char *s)
{
	for (int i = 0; i < PH_MAX; i++) if (cl[i].fd >= 0 && cl[i].subscribed) send_line(i, s);
}

static void give_back(void)
{
	if (owner < 0) return;
	owner = -1;
	if (is_menu()) menu_phosphor_screen(0);
	printf("phosphor: screen back to Main\n");
}

static void drop(int i)
{
	close(cl[i].fd);
	cl[i].fd = -1;
	if (owner == i) give_back();      // the shell went away: Main's menu comes back
	if (sheet_client == i) sheet_client = -1;
}

static void status_line(char *out, size_t n)
{
	char core[128];
	json_str(core, sizeof(core), user_io_get_core_name());
	snprintf(out, n, "{\"ok\":true,\"core\":\"%s\",\"menu\":%s,\"osd\":%s,\"fb\":%s,\"owner\":\"%s\",\"sheet\":%s}",
		core, is_menu() ? "true" : "false", user_io_osd_is_visible() ? "true" : "false",
		video_fb_state() ? "true" : "false", owner >= 0 ? "phosphor" : "main", sheet_client >= 0 ? "true" : "false");
}

static void command(int i, char *line)
{
	char out[512];
	while (*line == ' ') line++;
	if (!strcmp(line, "hello"))
		send_line(i, "{\"ok\":true,\"api\":1,\"name\":\"phosphor-main\"}");
	else if (!strcmp(line, "status"))
	{
		status_line(out, sizeof(out));
		send_line(i, out);
	}
	else if (!strcmp(line, "subscribe"))
	{
		cl[i].subscribed = 1;
		send_line(i, "{\"ok\":true}");
	}
	else if (!strcmp(line, "screen phosphor"))
	{
		if (owner >= 0 && owner != i) { send_line(i, "{\"ok\":false,\"error\":\"another client has the screen\"}"); return; }
		owner = i;
		if (is_menu()) menu_phosphor_screen(1);
		send_line(i, "{\"ok\":true}");
	}
	else if (!strcmp(line, "screen main"))
	{
		if (owner == i) give_back();
		send_line(i, "{\"ok\":true}");
	}
	else if (!strcmp(line, "sheet on")) { sheet_client = i; send_line(i, "{\"ok\":true}"); }
	else if (!strcmp(line, "sheet off")) { if (sheet_client == i) sheet_client = -1; send_line(i, "{\"ok\":true}"); }
	else if (!strncmp(line, "launch ", 7))
	{
		char real[PATH_MAX];
		const char *p = line + 7;
		struct stat st;
		if (!realpath(p, real) || strncmp(real, "/media/", 7) || stat(real, &st) || !S_ISREG(st.st_mode))
		{
			send_line(i, "{\"ok\":false,\"error\":\"no such file under /media\"}");
			return;
		}
		send_line(i, "{\"ok\":true}");
		if (isXmlName(real)) xml_load(real);
		else fpga_load_rbf(real);
	}
	else send_line(i, "{\"ok\":false,\"error\":\"unknown command\"}");
}

static void read_client(int i)
{
	for (;;)
	{
		ph_client_t *c = &cl[i];
		ssize_t r = recv(c->fd, c->buf + c->len, sizeof(c->buf) - 1 - c->len, MSG_DONTWAIT);
		if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) { drop(i); return; }
		if (r < 0) return;
		c->len += (size_t)r;
		c->buf[c->len] = 0;
		char *nl;
		while (cl[i].fd >= 0 && (nl = (char *)memchr(c->buf, '\n', c->len)))
		{
			*nl = 0;
			if (nl > c->buf && nl[-1] == '\r') nl[-1] = 0;
			command(i, c->buf);
			if (cl[i].fd < 0) return;
			size_t used = (size_t)(nl - c->buf) + 1;
			memmove(c->buf, nl + 1, c->len - used);
			c->len -= used;
		}
		if (c->len >= sizeof(c->buf) - 1) { drop(i); return; }   // a line that never ends
	}
}

void phosphor_poll(void)
{
	if (!inited) ph_init();
	if (lfd < 0) return;

	int fd;
	while ((fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0)
	{
		int slot = -1;
		for (int i = 0; i < PH_MAX; i++) if (cl[i].fd < 0) { slot = i; break; }
		if (slot < 0) { close(fd); continue; }
		cl[slot].fd = fd;
		cl[slot].subscribed = 0;
		cl[slot].len = 0;
	}
	for (int i = 0; i < PH_MAX; i++) if (cl[i].fd >= 0) read_client(i);

	// events
	char core[64], ev[256], name[128];
	snprintf(core, sizeof(core), "%s", user_io_get_core_name());
	if (strcmp(core, last_core))
	{
		snprintf(last_core, sizeof(last_core), "%s", core);
		json_str(name, sizeof(name), core);
		snprintf(ev, sizeof(ev), "{\"event\":\"core\",\"name\":\"%s\",\"menu\":%s}", name, is_menu() ? "true" : "false");
		event_all(ev);
		if (owner >= 0 && is_menu()) menu_phosphor_screen(1);     // back from a game: the shell again
	}
	int osd = user_io_osd_is_visible() ? 1 : 0;
	if (osd != last_osd)
	{
		last_osd = osd;
		snprintf(ev, sizeof(ev), "{\"event\":\"osd\",\"open\":%s}", osd ? "true" : "false");
		event_all(ev);
	}
	// the menu core reloads its video after some actions: keep the shell's framebuffer up
	if (owner >= 0 && is_menu() && !video_fb_state()) menu_phosphor_screen(1);
}

uint32_t phosphor_filter_key(uint32_t c)
{
	if (!c) return c;
	uint32_t key = c & ~UPSTROKE;

	if (owner >= 0 && is_menu())
	{
		// the shell has the controls: Main's menu does nothing (no OSD, no F9, no file browser)
		if (key == KEY_F12 && (c & UPSTROKE)) event_all("{\"event\":\"menu_button\"}");
		return 0;
	}

	if (sheet_client >= 0 && !is_menu() && key == KEY_F12 && !user_io_osd_is_visible())
	{
		if (!(c & UPSTROKE))
		{
			if (!f12_down) f12_down = GetTimer(0);     // (held keys repeat: keep the first press)
			return 0;                                  // wait: short or long press?
		}
		unsigned long held = f12_down ? GetTimer(0) - f12_down : PH_LONG_PRESS_MS;
		f12_down = 0;
		if (held < PH_LONG_PRESS_MS)
		{
			event_all("{\"event\":\"menu_button\"}");   // the in-game sheet
			return 0;
		}
		return c;                                      // long press: the OSD, as usual
	}
	return c;
}
