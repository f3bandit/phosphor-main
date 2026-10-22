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
//   (Main's OSD list also has "Launch Phosphor", under Video processing: phosphor_start)
//   subscribe              this connection also gets events:
//                          {"event":"core","name":"SNES"} {"event":"menu_button"} {"event":"osd","open":true}
//
// The front ports (PlayStation controllers and memory cards over SNAC), with the Phosphor menu core
// (phosphor-menu) loaded; the stock menu core doesn't have them ("available":false):
//   ports                  {"ok":true,"available":true,"on":false}
//   ports on|off           the user port goes to the front ports (MT32-pi / MIDI off) or back. On
//                          belongs to this connection: if it closes, the ports go off again.
//   pads                   {"ok":true,"pads":[{"port":1,"connected":true,"id":115,"buttons":16,
//                          "rx":128,"ry":128,"lx":128,"ly":128},...]}   id 65 digital, 115 analog;
//                          buttons pressed = 1: bit 0 Select, L3, R3, Start, Up, Right, Down, Left,
//                          L2, R2, L1, R1, Triangle, Circle, Cross, bit 15 Square
//   pad_analog 1|2         switch that port's DualShock to analog (it answers right away)
//   card_read P S          sector S (0..1023) of the card in port P: {"ok":true,"data":"<256 hex>"}
//   card_write P S HEX     write 128 bytes (256 hex digits) to sector S: {"ok":true}
//                          errors: "no card", "bad reply", "out of range", "timeout", "busy"
//                          (one card operation at a time; the reply comes when it's done)
// Apart from those commands nothing here talks to the FPGA directly: it uses Main's own
// functions, from Main's UI loop.
#include "phosphor.h"
#include "menu.h"
#include "user_io.h"
#include "video.h"
#include "fpga_io.h"
#include "file_io.h"
#include "input.h"
#include "hardware.h"
#include "spi.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
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

// the front ports (phosphor-menu's extension on EXT_BUS)
#define PH_ID          0x80
#define PH_MODE        0x81
#define PH_PADS        0x82
#define PH_CARD        0x83
#define PH_CARD_STATUS 0x84
#define PH_PAD_CMD     0x85
#define PH_CARD_TIMEOUT_MS 500
static int ext_ver = -1;         // -1 not asked yet (for this core), 0 none, >0 the extension's version
static int ports_client = -1;    // the client that turned the front ports on
static struct { int client, write; unsigned long start, next; } card_op = { -1, 0, 0, 0 };
static struct { int port, step; unsigned long next; } pad_seq = { 0, 0, 0 };

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

static int ext_present(void)
{
	if (!is_menu()) return 0;
	if (ext_ver < 0)
	{
		spi_uio_cmd_cont(PH_ID);
		uint16_t id = spi_w(0);
		uint16_t ver = spi_w(0);
		DisableIO();
		ext_ver = (id == 0x5048 && ver) ? ver : 0;
		printf("phosphor: front ports %s\n", ext_ver ? "available" : "not in this menu core");
	}
	return ext_ver > 0;
}

static void ports_set(int client, int on)
{
	ports_client = on ? client : -1;
	if (!on) { pad_seq.step = 0; }
	if (!ext_present()) return;
	spi_uio_cmd_cont(PH_MODE);
	spi_w(on ? 1 : 0);
	DisableIO();
}

static const char *card_error(int r)
{
	return r == 1 ? "no card" : r == 3 ? "out of range" : "bad reply";
}

static void pads_reply(int i)
{
	uint16_t w[9];
	spi_uio_cmd_cont(PH_PADS);
	for (int k = 0; k < 9; k++) w[k] = spi_w(0);
	DisableIO();
	if (ports_client >= 0 && !(w[0] & 1)) ports_set(ports_client, 1);   // the menu core was reloaded
	char out[512];
	size_t n = snprintf(out, sizeof(out), "{\"ok\":true,\"on\":%s,\"pads\":[", (w[0] & 1) ? "true" : "false");
	for (int p = 0; p < 2; p++)
	{
		uint16_t *q = w + 1 + p * 4;
		int conn = (w[0] & 1) && (q[0] & 0x100);
		n += snprintf(out + n, sizeof(out) - n, "%s{\"port\":%d,\"connected\":%s,\"id\":%d,\"buttons\":%d,\"rx\":%d,\"ry\":%d,\"lx\":%d,\"ly\":%d}",
			p ? "," : "", p + 1, conn ? "true" : "false", conn ? (q[0] & 0xFF) : 0, conn ? (uint16_t)~q[1] : 0,
			q[2] & 0xFF, q[2] >> 8, q[3] & 0xFF, q[3] >> 8);
	}
	snprintf(out + n, sizeof(out) - n, "]}");
	send_line(i, out);
}

static int hexval(char c)
{
	return (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
}

// "card_read P S" / "card_write P S HEX": starts the operation; card_poll() replies when it's done
static void card_command(int i, char *args, int write)
{
	if (!ext_present()) { send_line(i, "{\"ok\":false,\"error\":\"front ports not available\"}"); return; }
	if (ports_client < 0) { send_line(i, "{\"ok\":false,\"error\":\"front ports are off\"}"); return; }
	if (card_op.client >= 0) { send_line(i, "{\"ok\":false,\"error\":\"busy\"}"); return; }
	char *end;
	long port = strtol(args, &end, 10);
	if (end == args || (port != 1 && port != 2)) { send_line(i, "{\"ok\":false,\"error\":\"port is 1 or 2\"}"); return; }
	args = end;
	long sec = strtol(args, &end, 10);
	if (end == args || sec < 0 || sec > 1023) { send_line(i, "{\"ok\":false,\"error\":\"sector is 0..1023\"}"); return; }
	uint16_t data[64] = {};
	if (write)
	{
		while (*end == ' ') end++;
		if (strlen(end) != 256) { send_line(i, "{\"ok\":false,\"error\":\"data is 256 hex digits\"}"); return; }
		for (int k = 0; k < 128; k++)
		{
			int h = hexval(end[2 * k]), l = hexval(end[2 * k + 1]);
			if (h < 0 || l < 0) { send_line(i, "{\"ok\":false,\"error\":\"data is 256 hex digits\"}"); return; }
			data[k / 2] |= (uint16_t)((h << 4 | l) << ((k & 1) * 8));
		}
	}
	spi_uio_cmd_cont(PH_CARD);
	spi_w((port == 2 ? 0x100 : 0) | (write ? 1 : 0));
	spi_w((uint16_t)sec);
	if (write) for (int k = 0; k < 64; k++) spi_w(data[k]);
	DisableIO();
	card_op.client = i;
	card_op.write = write;
	card_op.start = GetTimer(0);
	card_op.next = GetTimer(5);     // a read takes about 5 ms on the bus
}

static void card_poll(void)
{
	if (card_op.client < 0 || GetTimer(0) < card_op.next) return;
	int i = card_op.client;
	if (!ext_present())   // another core was loaded
	{
		card_op.client = -1;
		send_line(i, "{\"ok\":false,\"error\":\"front ports not available\"}");
		return;
	}
	spi_uio_cmd_cont(PH_CARD_STATUS);
	uint16_t st = spi_w(0);
	if (st & 0x8000)
	{
		DisableIO();
		card_op.next = GetTimer(2);
		if (GetTimer(0) - card_op.start > PH_CARD_TIMEOUT_MS)
		{
			card_op.client = -1;
			send_line(i, "{\"ok\":false,\"error\":\"timeout\"}");
		}
		return;
	}
	card_op.client = -1;
	if (!(st & 0x4000)) { DisableIO(); send_line(i, "{\"ok\":false,\"error\":\"timeout\"}"); return; }
	if (st & 3)
	{
		DisableIO();
		char out[96];
		snprintf(out, sizeof(out), "{\"ok\":false,\"error\":\"%s\"}", card_error(st & 3));
		send_line(i, out);
		return;
	}
	if (card_op.write) { DisableIO(); send_line(i, "{\"ok\":true}"); return; }
	char out[300] = "{\"ok\":true,\"data\":\"";
	size_t n = strlen(out);
	static const char hx[] = "0123456789abcdef";
	for (int k = 0; k < 64; k++)
	{
		uint16_t w = spi_w(0);
		uint8_t b0 = w & 0xFF, b1 = w >> 8;
		out[n++] = hx[b0 >> 4]; out[n++] = hx[b0 & 15];
		out[n++] = hx[b1 >> 4]; out[n++] = hx[b1 & 15];
	}
	DisableIO();
	snprintf(out + n, sizeof(out) - n, "\"}");
	send_line(i, out);
}

// switching a DualShock to analog: config mode, analog on, leave config, a little apart
static void pad_seq_poll(void)
{
	if (!pad_seq.step || GetTimer(0) < pad_seq.next) return;
	if (ports_client < 0 || !ext_present()) { pad_seq.step = 0; return; }
	spi_uio_cmd_cont(PH_PAD_CMD);
	spi_w((pad_seq.port == 2 ? 0x100 : 0) | pad_seq.step);
	DisableIO();
	pad_seq.step = pad_seq.step == 3 ? 0 : pad_seq.step + 1;
	pad_seq.next = GetTimer(20);
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
	if (card_op.client == i) card_op.client = -1;   // (the card finishes on its own)
	if (ports_client == i) ports_set(-1, 0);
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
	else if (!strcmp(line, "ports"))
	{
		snprintf(out, sizeof(out), "{\"ok\":true,\"available\":%s,\"on\":%s}", ext_present() ? "true" : "false",
			ports_client >= 0 ? "true" : "false");
		send_line(i, out);
	}
	else if (!strcmp(line, "ports on") || !strcmp(line, "ports off"))
	{
		int on = line[7] == 'n';
		if (!ext_present()) { send_line(i, "{\"ok\":false,\"error\":\"front ports not available\"}"); return; }
		if (ports_client >= 0 && ports_client != i) { send_line(i, "{\"ok\":false,\"error\":\"another client has the front ports\"}"); return; }
		ports_set(i, on);
		send_line(i, "{\"ok\":true}");
	}
	else if (!strcmp(line, "pads"))
	{
		if (!ext_present()) { send_line(i, "{\"ok\":false,\"error\":\"front ports not available\"}"); return; }
		pads_reply(i);
	}
	else if (!strcmp(line, "pad_analog 1") || !strcmp(line, "pad_analog 2"))
	{
		if (!ext_present() || ports_client < 0) { send_line(i, "{\"ok\":false,\"error\":\"front ports are off\"}"); return; }
		pad_seq.port = line[11] - '0';
		pad_seq.step = 1;
		pad_seq.next = 0;
		send_line(i, "{\"ok\":true}");
	}
	else if (!strncmp(line, "card_read ", 10)) card_command(i, line + 10, 0);
	else if (!strncmp(line, "card_write ", 11)) card_command(i, line + 11, 1);
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

	card_poll();
	pad_seq_poll();

	// events
	char core[64], ev[256], name[128];
	snprintf(core, sizeof(core), "%s", user_io_get_core_name());
	if (strcmp(core, last_core))
	{
		snprintf(last_core, sizeof(last_core), "%s", core);
		ext_ver = -1;                       // another core (or the menu core again): ask again
		if (ports_client >= 0 && !is_menu()) { ports_client = -1; pad_seq.step = 0; }
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

#define PH_SCRIPT "/media/fat/phosphor/phosphor.sh"

const char *phosphor_menu_core(void)
{
	static const char *p = "/media/fat/phosphor/cores/menu_phosphor.rbf";
	struct stat st;
	// a whole core is a few MB; anything else (a failed copy) is ignored and the stock one loads
	return (!stat(p, &st) && S_ISREG(st.st_mode) && st.st_size > 1000000) ? p : NULL;
}

int phosphor_installed(void)
{
	return access(PH_SCRIPT, R_OK) == 0;
}

int phosphor_start(void)
{
	if (!phosphor_installed()) return 0;
	if (owner >= 0)
	{
		// the shell is running: in the menu it takes the screen again, from a game the menu core loads
		// and it takes the screen when Main has restarted
		if (is_menu()) menu_phosphor_screen(1);
		else fpga_load_rbf("menu.rbf");
		return 1;
	}
	int from_game = !is_menu();
	printf("phosphor: starting %s%s\n", PH_SCRIPT, from_game ? " (after loading the menu core)" : "");
	pid_t pid = fork();
	if (pid < 0) return 0;
	if (pid == 0)
	{
		// detached, so it outlives this Main (core loads restart Main) and leaves no zombie
		setsid();
		if (fork() != 0) _exit(0);
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); if (nul > 2) close(nul); }
		for (int fd = 3; fd < 256; fd++) close(fd);
		if (from_game) sleep(4);    // the menu core loads and Main restarts first
		execl("/bin/sh", "sh", PH_SCRIPT, "start", (char *)NULL);
		_exit(127);
	}
	waitpid(pid, NULL, 0);
	if (from_game) fpga_load_rbf("menu.rbf");
	return 1;
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
