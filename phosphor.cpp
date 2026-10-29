// Phosphor: a small local API so the Phosphor shell and phosphord can work with Main instead of
// around it. Part of phosphor-main (a fork of MiSTer-devel/Main_MiSTer), GPL-3.0 like Main.
//
// Unix socket /tmp/phosphor-main.sock (root only), one command per line, one JSON reply per line:
//   hello                  {"ok":true,"api":7,"name":"phosphor-main","pads":true,"scripts":true,"mapping":true,
//                          "disc":true,"ingame":true}
//   status                 core, menu core or not, OSD visible, framebuffer shown, who owns the screen
//   script PATH            (the shell, owning the screen) run a script the way MiSTer's Scripts menu does:
//                          MiSTer's terminal on tty2, controllers as its keys, "Press any key to continue";
//                          then event {"event":"script_done","status":N} and the shell has the screen again
//   screen phosphor|main   Phosphor takes the screen in the menu core (Linux framebuffer shown, Main's
//                          menu ignores keys) or gives it back. Ownership belongs to this connection:
//                          if it closes (the shell quit or crashed), Main takes the screen back.
//   sheet on|off           in a game, a short menu-button press goes to Phosphor (event), a long one
//                          (0.7 s) opens the OSD as usual; off: the OSD as usual
//   ingame on|off          in a game: Phosphor's screen (the Linux framebuffer) shown over the running game,
//                          for Phosphor's in-game menu; the controllers go to Phosphor (pad events), not the
//                          game, until off. The game keeps running behind it. Belongs to this connection
//                          (it becomes the screen's owner); a core change or the connection closing ends it,
//                          and {"event":"ingame","on":false} tells it when Main took the screen back.
//   ingame text on         the same for a CRT (MiSTer's analog out shows the core and the OSD, never the
//                          framebuffer): then osdmenu SEL TITLE<0x1f>line<0x1f>line... draws Phosphor's in-game
//                          menu as text in the OSD, row SEL inverted; ingame off ends it
//   ingame osd             from Phosphor's in-game menu to MiSTer's menu (the core's own options)
//   disc / disc eject      the dock's CD/DVD drive: what's in it, and open the tray (phosphor_disc.cpp);
//                          subscribers get {"event":"disc",...} when it changes
//   launch <path>          load a core (.rbf) or a .mgl/.mra, like MiSTer_cmd load_core (under /media only)
//   (Main's OSD has "Launch Phosphor": first on MiSTer's System page, and on a core's System page under
//   Video processing; holding the menu button 2 s in MiSTer's menu does the same: phosphor_start)
//   subscribe              this connection also gets events:
//                          {"event":"core","name":"SNES"} {"event":"menu_button"} {"event":"osd","open":true}
//                          {"event":"show"}: Launch Phosphor was chosen in Main's menu while the shell
//                          had given the screen back (the shell takes it again)
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
// Controller button mapping, MiSTer's own (the menu's "Define joystick buttons", same mapping file),
// with the shell showing the steps instead of the OSD (the screen owner only, in the menu core):
//   map_start [VVVV:PPPP]  start; only that controller counts (else whichever is pressed first)
//   map_skip               skip this step (the D-pad test, a stick, X/Y/L/R/Select/Start)
//   map_cancel             stop, nothing saved (the controller keeps its old mapping); so do Esc on a
//                          keyboard and Select + Start together on the controller
//   map_state              {"ok":true,"on":true,"step":N,"dev":"2dc8:6013"}
//   events to the owner:   {"event":"map","step":N,"dev":"2dc8:6013"} at each step, then
//                          {"event":"map","done":true,"saved":true|false,"file":"input_2dc8_6013_v3.map"}
//   steps (MiSTer's): -6 -5 D-pad test right, down; -4 -3 stick 1 right, down; -2 -1 stick 2;
//   0..3 right left down up; 4..11 A B X Y L R Select Start; 20 Menu (the OSD button, a 2-button
//   combo allowed); 21 OK; 22 Back; 23 24 a stick for the menu, right, down. On a stick step any
//   button means "no such stick"; on X..Start, a button already used skips it. Mouse emulation
//   (12..19) isn't asked.
// Apart from those commands nothing here talks to the FPGA directly: it uses Main's own
// functions, from Main's UI loop.
// Copyright (C) 2026 f3bandit. GPL-3.0 (see LICENSE).
#include "phosphor.h"
#include "phosphor_disc.h"
#include "scaler.h"
#include "achievements.h"
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
#include <time.h>

#include "support/arcade/mra_loader.h"          // xml_load: .mra / .mgl
void menu_phosphor_screen(int phosphor);        // menu.cpp

#define PH_SOCK "/tmp/phosphor-main.sock"
#define PH_MAX 4
#define PH_LINE 1024

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
static int ingame_client = -1;   // Phosphor's screen is shown over the running game for this client
static int ingame_text;          // ...as text in MiSTer's OSD (a CRT: "ingame text on"), not the framebuffer
static uint32_t rep_mask;        // the held direction repeated to the shell (phosphor_pad), 0: none
static unsigned long rep_next;
static char last_core[64];
static int last_osd = -1;
static unsigned long f12_down;   // when the menu button went down (sheet), 0: not held
static int osd_direct;             // Main's own menu key press and release, from "ingame osd": straight through
static unsigned long osd_release;  // when to send that release (MiSTer's menu opens the OSD on the release)
static int inited;
static unsigned long hold_until;   // Main's file browser waits for the shell until then (0: no wait)

// controller mapping (input.cpp)
extern int phosphor_mapping;
extern uint32_t phosphor_map_vidpid;
extern int phosphor_map_cancel;
int input_map_running(void);
void input_map_jump(int to);
uint32_t input_map_dev_vidpid(void);
const char *input_map_file(void);
static struct { int client, step; uint32_t dev; } mp = { -1, -100, 0 };
#define MAP_COUNT 27      // MiSTer's menu mapping: 21 buttons and the 6 test steps

// the front ports (phosphor-menu's extension on EXT_BUS)
#define PH_ID          0x80
#define PH_MODE        0x81
#define PH_PADS        0x82
#define PH_CARD        0x83
#define PH_CARD_STATUS 0x84
#define PH_PAD_CMD     0x85
#define PH_LOOK        0x86   // bit 0: MiSTer's static behind the menu (else black)
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

static void event_all(const char *s);
static void ph_init(void)
{
	inited = 1;
	achievements_phosphor_events(event_all);      // RetroAchievements: login and unlock events
	if (phosphor_installed()) hold_until = GetTimer(60000);
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

static void drop(int i);
static void send_line(int i, const char *s);

static void map_dev(char *out, size_t n, uint32_t vp)
{
	if (vp) snprintf(out, n, "%04x:%04x", vp >> 16, vp & 0xFFFF);
	else out[0] = 0;
}

static void map_end(int saved)
{
	char ev[256], file[128];
	snprintf(file, sizeof(file), "%s", saved ? input_map_file() : "");
	finish_map_setting(saved ? 0 : 1);
	phosphor_mapping = 0;
	phosphor_map_vidpid = 0;
	if (mp.client >= 0 && cl[mp.client].fd >= 0)
	{
		snprintf(ev, sizeof(ev), "{\"event\":\"map\",\"done\":true,\"saved\":%s,\"file\":\"%s\"}", saved ? "true" : "false", file);
		send_line(mp.client, ev);
	}
	mp.client = -1;
	mp.step = -100;
}

static void map_command(int i, const char *arg)
{
	char out[160], dev[16];
	if (!strcmp(arg, "state"))
	{
		map_dev(dev, sizeof(dev), input_map_dev_vidpid());
		snprintf(out, sizeof(out), "{\"ok\":true,\"on\":%s,\"step\":%d,\"dev\":\"%s\"}", phosphor_mapping ? "true" : "false",
			phosphor_mapping ? get_map_button() : 0, phosphor_mapping ? dev : "");
		send_line(i, out);
		return;
	}
	if (owner != i || !is_menu()) { send_line(i, "{\"ok\":false,\"error\":\"take the screen first (screen phosphor)\"}"); return; }
	if (!strncmp(arg, "start", 5) && (!arg[5] || arg[5] == ' '))
	{
		if (input_map_running()) { send_line(i, "{\"ok\":false,\"error\":\"a mapping is already running\"}"); return; }
		unsigned v = 0, p = 0;
		if (arg[5] == ' ' && sscanf(arg + 6, "%4x:%4x", &v, &p) != 2) { send_line(i, "{\"ok\":false,\"error\":\"map_start VVVV:PPPP\"}"); return; }
		// the names MiSTer's menu gives the buttons (MENU_JOYSYSMAP)
		static const char *const N[] = { "A", "B", "X", "Y", "L", "R", "Select", "Start", "Mouse Move RIGHT", "Mouse Move LEFT",
			"Mouse Move DOWN", "Mouse Move UP", "Mouse Btn Left", "Mouse Btn Right", "Mouse Btn Middle", "Mouse Emu/Sniper", "Menu",
			"Menu: OK", "Menu: Back" };
		for (unsigned k = 0; k < sizeof(N) / sizeof(N[0]); k++) snprintf(joy_bnames[k], sizeof(joy_bnames[k]), "%s", N[k]);
		joy_bcount = 21;
		phosphor_map_vidpid = (v << 16) | p;
		phosphor_mapping = 1;
		mp.client = i;
		mp.step = -100;
		mp.dev = 0;
		start_map_setting(MAP_COUNT);
		send_line(i, "{\"ok\":true}");
		return;
	}
	if (!phosphor_mapping) { send_line(i, "{\"ok\":false,\"error\":\"no mapping running\"}"); return; }
	if (!strcmp(arg, "cancel")) { send_line(i, "{\"ok\":true}"); map_end(0); return; }
	if (!strcmp(arg, "skip"))
	{
		int b = get_map_button(), to;
		if (b < -4) to = -4;                        // no D-pad test: straight to the sticks
		else if (b < 0) to = b < -2 ? -2 : 0;       // no such stick
		else if (b >= 6 && b <= 11) to = b + 1;     // X, Y, L, R, Select, Start
		else if (b == 23 || b == 24) to = 25;       // no stick for the menu
		else { send_line(i, "{\"ok\":false,\"error\":\"this one is needed\"}"); return; }
		input_map_jump(to);
		send_line(i, "{\"ok\":true}");
		return;
	}
	send_line(i, "{\"ok\":false,\"error\":\"map_start, map_skip, map_cancel or map_state\"}");
}

// each loop: skip what isn't asked, finish at the end, and tell the shell the step
static void map_poll(void)
{
	if (!phosphor_mapping) { phosphor_map_cancel = 0; return; }
	if (phosphor_map_cancel || !input_map_running()) { phosphor_map_cancel = 0; map_end(0); return; }   // cancelled, or stopped some other way
	int b = get_map_button();
	if (b >= 12 && b <= 19) { input_map_jump(20); b = 20; }       // mouse emulation: not asked
	if (b >= 25) { map_end(1); return; }
	uint32_t d = input_map_dev_vidpid();
	if (b != mp.step || d != mp.dev)
	{
		char ev[128], dev[16];
		mp.step = b;
		mp.dev = d;
		map_dev(dev, sizeof(dev), d);
		snprintf(ev, sizeof(ev), "{\"event\":\"map\",\"step\":%d,\"dev\":\"%s\"}", b, dev);
		if (mp.client >= 0 && cl[mp.client].fd >= 0) send_line(mp.client, ev);
	}
}

static void send_line(int i, const char *s)
{
	if (cl[i].fd < 0) return;
	size_t n = strlen(s);
	// small replies on a local socket: if it can't take a whole line now, the client isn't reading
	if (send(cl[i].fd, s, n, MSG_NOSIGNAL | MSG_DONTWAIT) != (ssize_t)n || send(cl[i].fd, "\n", 1, MSG_NOSIGNAL | MSG_DONTWAIT) != 1)
	{
		printf("phosphor: client %d not reading, dropped\n", i);
		drop(i);     // gives back what it owned (screen, front ports, card operation)
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
	// asked again (at most once a second) while the answer is "no": right after Main starts the core
	// may not answer yet, and a "no" kept for good made phosphord load the core again (a restart)
	static unsigned long next_probe;
	if (ext_ver == 0 && CheckTimer(next_probe)) ext_ver = -1;
	if (ext_ver < 0)
	{
		next_probe = GetTimer(1000);
		// the core's answer to a word comes back with that same word (as with hps_io's own commands,
		// e.g. UIO_GET_SDSTAT): the command word itself returns the first reply word
		uint16_t id = spi_uio_cmd_cont(PH_ID);
		uint16_t ver = spi_w(0);
		DisableIO();
		ext_ver = (id == 0x5048 && ver) ? ver : 0;
		static int said = -1;
		if (said != (ext_ver > 0)) printf("phosphor: front ports %s\n", ext_ver ? "available" : "not in this menu core");
		said = ext_ver > 0;
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
	w[0] = spi_uio_cmd_cont(PH_PADS);
	for (int k = 1; k < 9; k++) w[k] = spi_w(0);
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
	uint16_t st = spi_uio_cmd_cont(PH_CARD_STATUS);
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

// ---- power check markers (read by the health-watch module at start-up): a script runs -> "script";
// Main restarts the system on purpose -> ".clean" with this boot's id. Left behind from another boot
// with no .clean, a marker means the power went mid-job (a brownout leaves no other trace).
#define BUSY_DIR "/media/fat/phosphor/state/busy"
static const char *boot_id(void)
{
	static char id[64];
	if (!id[0])
	{
		FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
		if (f) { if (!fgets(id, sizeof(id), f)) id[0] = 0; fclose(f); }
		id[strcspn(id, "\n")] = 0;
	}
	return id;
}

static void busy_write(const char *name, const char *what)
{
	if (!boot_id()[0] || access("/media/fat/phosphor", F_OK)) return;
	mkdir("/media/fat/phosphor/state", 0755);
	mkdir(BUSY_DIR, 0755);
	char p[300], buf[300];
	snprintf(p, sizeof(p), BUSY_DIR "/%s", name);
	int n = what ? snprintf(buf, sizeof(buf), "boot=%s\nat=%ld\nwhat=%s\n", boot_id(), (long)time(NULL), what)
	             : snprintf(buf, sizeof(buf), "boot=%s\n", boot_id());
	int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) return;
	if (write(fd, buf, n) != n) printf("phosphor: couldn't write %s\n", p);
	fsync(fd);
	close(fd);
	int d = open(BUSY_DIR, O_RDONLY | O_CLOEXEC);
	if (d >= 0) { fsync(d); close(d); }
}

void phosphor_mark_clean(void) { busy_write(".clean", NULL); }

// Main started again in the same boot (it was restarted mid-script): that marker is stale
static void busy_init(void)
{
	FILE *f = fopen(BUSY_DIR "/script", "r");
	if (!f) return;
	char line[160] = "";
	int same = 0;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, "boot=", 5)) { line[strcspn(line, "\n")] = 0; same = boot_id()[0] && !strcmp(line + 5, boot_id()); break; }
	fclose(f);
	if (same) unlink(BUSY_DIR "/script");
}

// ---- scripts (Update All, the downloader, Scripts/*.sh), run the way MiSTer's Scripts menu runs them
static pid_t script_pid;
static int script_state, script_client = -1, script_status;   // state: 0 none, 1 running, 2 "press any key"
int video_chvt(int num);

static void script_finish(void)
{
	script_state = 0;
	unlink(BUSY_DIR "/script");
	video_chvt(1);
	video_fb_console_done();
	if (script_client >= 0 && cl[script_client].fd >= 0)
	{
		char ev[80];
		snprintf(ev, sizeof(ev), "{\"event\":\"script_done\",\"status\":%d}", script_status);
		send_line(script_client, ev);
	}
	script_client = -1;
}

static void script_start(int i, const char *path)
{
	struct stat sb;
	if (owner != i || !is_menu()) { send_line(i, "{\"ok\":false,\"error\":\"take the screen first (screen phosphor)\"}"); return; }
	if (script_state) { send_line(i, "{\"ok\":false,\"error\":\"a script is running\"}"); return; }
	if (strncmp(path, "/media/", 7) || strstr(path, "..") || strchr(path, '"') || strchr(path, '$') || strchr(path, '`')
		|| stat(path, &sb) || !S_ISREG(sb.st_mode))
	{
		send_line(i, "{\"ok\":false,\"error\":\"no such script\"}");
		return;
	}
	static char cmd[2048];
	snprintf(cmd, sizeof(cmd), "#!/bin/bash\nexport LC_ALL=en_US.UTF-8\nexport HOME=/root\ncd \"$(dirname \"%s\")\"\n\"%s\"\n"
		"EXITSTATUS=$?\necho \"Press any key to continue\"\nexit $EXITSTATUS\n", path, path);
	unlink("/tmp/phosphor-script");
	int fd = open("/tmp/phosphor-script", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
	if (fd < 0 || write(fd, cmd, strlen(cmd)) != (ssize_t)strlen(cmd)) { if (fd >= 0) close(fd); send_line(i, "{\"ok\":false,\"error\":\"can't write /tmp\"}"); return; }
	close(fd);
	video_fb_console();
	video_chvt(2);
	script_pid = fork();
	if (!script_pid)
	{
		execl("/sbin/agetty", "/sbin/agetty", "-a", "root", "-l", "/tmp/phosphor-script", "--nohostname", "-L", "tty2", "linux", NULL);
		_exit(127);
	}
	if (script_pid < 0) { video_chvt(1); video_fb_console_done(); send_line(i, "{\"ok\":false,\"error\":\"can't start it\"}"); return; }
	script_state = 1;
	{
		const char *b = strrchr(path, '/');
		b = b ? b + 1 : path;
		char what[120];
		if (!strcmp(b, "update_all.sh")) snprintf(what, sizeof(what), "Update All");
		else if (!strcmp(b, "update.sh")) snprintf(what, sizeof(what), "Update");
		else snprintf(what, sizeof(what), "Script %.*s", (int)(strlen(b) > 3 && !strcmp(b + strlen(b) - 3, ".sh") ? strlen(b) - 3 : strlen(b)), b);
		busy_write("script", what);
	}
	script_client = i;
	script_status = 0;
	send_line(i, "{\"ok\":true}");
}

static void script_poll(void)
{
	int st;
	if (script_state == 1 && waitpid(script_pid, &st, WNOHANG) > 0)
	{
		script_status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
		script_state = 2;                      // "Press any key to continue" is on the screen
		unlink(BUSY_DIR "/script");            // the work is over: switching off at the prompt is fine
	}
}

static uint32_t text_shown_at;      // text mode: when the OSD was last seen showing Phosphor's menu (+1 s)
static void ingame_end_text(void)
{
	text_shown_at = 0;
	if (ingame_text) { ingame_text = 0; phosphor_osd_menu_off(); }
}
int phosphor_ingame_text(void) { return ingame_text && ingame_client >= 0; }

static void drop(int i)
{
	close(cl[i].fd);
	cl[i].fd = -1;
	if (mp.client == i && phosphor_mapping) map_end(0);   // the shell went away mid-mapping: nothing saved
	if (owner == i) give_back();      // the shell went away: Main's menu comes back
	if (sheet_client == i) sheet_client = -1;
	if (ingame_client == i) { ingame_client = -1; rep_mask = 0; ingame_end_text(); if (!is_menu() && video_fb_state()) video_fb_enable(0); }
	if (card_op.client == i) card_op.client = -1;   // (the card finishes on its own)
	if (ports_client == i) ports_set(-1, 0);
	if (script_client == i) script_client = -1;   // (the script carries on; nobody to tell)
}

// ---------- screenshots: both sticks pressed in (L3 + R3) on a controller ("shot on|off"; on unless the
// shell says otherwise). In a game MiSTer's own screenshot (screenshots/<core>/ on the SD card); on
// Phosphor's screens the shell takes it (event {"event":"shot"}). The presses still reach the core:
// games hardly ever need both sticks in at once.
static int shot_on = 1;
static int shot_ours;                       // the screenshot in progress was the combo's (or the apps')
static uint8_t thumbs[64];                  // per device: bit 0 left stick in, bit 1 right stick in

void phosphor_thumbs(int dev, int right, int press)
{
	if (dev < 0 || dev >= 64) return;
	uint8_t was = thumbs[dev], bit = right ? 2 : 1;
	thumbs[dev] = press ? (was | bit) : (was & ~bit);
	if (!press || thumbs[dev] != 3 || was == 3 || !shot_on || user_io_osd_is_visible()) return;
	// Phosphor's picture on the screen (its menus, or its in-game menu over a game): the shell saves
	// that; MiSTer's capture would only see the game's video, which isn't what's shown
	if (owner >= 0 && video_fb_state()) event_all("{\"event\":\"shot\"}");
	else if (!is_menu())
	{
		shot_ours = 1;
		request_screenshot(NULL, 1);
	}
}

int phosphor_screenshot_result(const char *file, int ok)
{
	if (!shot_ours) return 0;
	shot_ours = 0;
	char ev[1400], f[1200];
	json_str(f, sizeof(f), file ? file : "");
	snprintf(ev, sizeof(ev), "{\"event\":\"screenshot\",\"ok\":%s,\"file\":\"%s\"}", ok ? "true" : "false", f);
	event_all(ev);
	Info(ok ? "Screenshot saved" : "Screenshot failed");
	return 1;
}

// the file loaded into the running core (Main restarts with each core, so this is always this core's)
static char cur_file[1024];
void phosphor_file_loaded(const char *path)
{
	snprintf(cur_file, sizeof(cur_file), "%s", path ? path : "");
}

static void status_line(char *out, size_t n)
{
	char core[128], file[2100];
	json_str(core, sizeof(core), user_io_get_core_name());
	json_str(file, sizeof(file), cur_file);
	snprintf(out, n, "{\"ok\":true,\"core\":\"%s\",\"menu\":%s,\"osd\":%s,\"fb\":%s,\"owner\":\"%s\",\"sheet\":%s,\"file\":\"%s\"}",
		core, is_menu() ? "true" : "false", user_io_osd_is_visible() ? "true" : "false",
		video_fb_state() ? "true" : "false", owner >= 0 ? "phosphor" : "main", sheet_client >= 0 ? "true" : "false", file);
}

/* Phosphor's CRT mode: Phosphor's own frames out of the analog port at a TV's own timing (15 kHz 240p /
   288p, 480i / 576i, or 31 kHz 480p / 576p), through Phosphor's menu core's native video path (from
   Retro-Remake's menu_ConsoleMode): status[9] on, [24:22] the mode, [15:10] / [21:16] the centering.
   The shell draws into the native buffer at the mode's size. Kept in phosphor/config/crt ("on mode h v")
   and set again on every menu core load (a new core starts with its status cleared). */
#define PH_CRT_CFG "/media/fat/phosphor/config/crt"
static struct { int on, mode, h, v, loaded, applied; } crt;

static void crt_load(void)
{
	if (crt.loaded) return;
	crt.loaded = 1;
	FILE *f = fopen(PH_CRT_CFG, "r");
	if (!f) return;
	int on = 0, mode = 0, h = 0, v = 0;
	if (fscanf(f, "%d %d %d %d", &on, &mode, &h, &v) == 4)
	{
		crt.on = on ? 1 : 0;
		crt.mode = mode & 7;
		crt.h = h < -31 ? -31 : h > 31 ? 31 : h;
		crt.v = v < -31 ? -31 : v > 31 ? 31 : v;
	}
	fclose(f);
}

static void crt_apply(void)
{
	if (!is_menu()) return;
	user_io_status_set("[24:22]", (uint32_t)crt.mode);
	user_io_status_set("[15:10]", (uint32_t)(crt.h & 63));
	user_io_status_set("[21:16]", (uint32_t)(crt.v & 63));
	user_io_status_set("[9]", crt.on ? 1 : 0);
	crt.applied = 1;
	printf("Phosphor: CRT mode %s (mode %d, centering %d %d)\n", crt.on ? "on" : "off", crt.mode, crt.h, crt.v);
}

static void crt_command(int i, const char *args)
{
	crt_load();
	int on, mode, h, v;
	if (args && sscanf(args, "%d %d %d %d", &on, &mode, &h, &v) == 4)
	{
		crt.on = on ? 1 : 0;
		crt.mode = mode & 7;
		crt.h = h < -31 ? -31 : h > 31 ? 31 : h;
		crt.v = v < -31 ? -31 : v > 31 ? 31 : v;
		FILE *f = fopen(PH_CRT_CFG ".tmp", "w");
		if (f)
		{
			fprintf(f, "%d %d %d %d\n", crt.on, crt.mode, crt.h, crt.v);
			fclose(f);
			rename(PH_CRT_CFG ".tmp", PH_CRT_CFG);
		}
		if (ext_present()) crt_apply();                  // (Phosphor's menu core only: the stock one has no such bits)
	}
	char out[160];
	snprintf(out, sizeof(out), "{\"ok\":true,\"crt\":{\"on\":%s,\"mode\":%d,\"h\":%d,\"v\":%d,\"menu\":%s}}",
		crt.on ? "true" : "false", crt.mode, crt.h, crt.v, is_menu() ? "true" : "false");
	send_line(i, out);
}

static void command(int i, char *line)
{
	char out[512];
	while (*line == ' ') line++;
	if (!strcmp(line, "hello"))
		send_line(i, "{\"ok\":true,\"api\":7,\"name\":\"phosphor-main\",\"pads\":true,\"scripts\":true,\"mapping\":true,\"disc\":true,\"ingame\":true,\"ra\":true,\"crt\":true}");   // api 3: scripts, 4: mapping, 5: disc, ingame, 6: ra, 7: crt
	else if (!strncmp(line, "script ", 7)) script_start(i, line + 7);
	else if (!strcmp(line, "status"))
	{
		char st[2600];
		status_line(st, sizeof(st));
		send_line(i, st);
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
		hold_until = 0;
		if (is_menu()) menu_phosphor_screen(1);
		send_line(i, "{\"ok\":true}");
	}
	else if (!strcmp(line, "screen main"))
	{
		hold_until = 0;
		if (owner == i) give_back();
		send_line(i, "{\"ok\":true}");
	}
	else if (!strcmp(line, "sheet on")) { sheet_client = i; send_line(i, "{\"ok\":true}"); }
	else if (!strcmp(line, "ingame on"))
	{
		if (is_menu()) { send_line(i, "{\"ok\":false,\"error\":\"no game is running\"}"); return; }
		if (owner >= 0 && owner != i) { send_line(i, "{\"ok\":false,\"error\":\"another client has the screen\"}"); return; }
		if (user_io_osd_is_visible()) menu_key_set(KEY_F12);   // MiSTer's menu closes first
		ingame_end_text();                              // (from the text in-game menu: that one off)
		owner = i;
		ingame_client = i;
		video_chvt(1);
		video_fb_enable(1);
		send_line(i, video_fb_state() ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"this core can't show Phosphor's screen\"}");
		if (!video_fb_state())
		{
			ingame_client = -1;
			send_line(i, "{\"event\":\"ingame\",\"on\":false,\"error\":\"this core can't show Phosphor's screen\"}");
		}
	}
	else if (!strcmp(line, "ingame text on"))
	{
		// the same, for a CRT: the menu drawn as text in MiSTer's OSD (osdmenu), the framebuffer left off
		if (is_menu()) { send_line(i, "{\"ok\":false,\"error\":\"no game is running\"}"); return; }
		if (owner >= 0 && owner != i) { send_line(i, "{\"ok\":false,\"error\":\"another client has the screen\"}"); return; }
		if (user_io_osd_is_visible()) menu_key_set(KEY_F12);
		if (video_fb_state()) video_fb_enable(0);       // (from the framebuffer in-game menu: that one off)
		owner = i;
		ingame_client = i;
		ingame_end_text();
		ingame_text = 1;
		send_line(i, "{\"ok\":true}");
	}
	else if (!strncmp(line, "osdmenu ", 8))
	{
		// osdmenu SEL TITLE<US>line<US>line...: Phosphor's in-game menu as OSD text, row SEL inverted
		if (ingame_client != i || !ingame_text) { send_line(i, "{\"ok\":false,\"error\":\"not in text mode\"}"); return; }
		char *t = line + 8, *end;
		long sel = strtol(t, &end, 10);
		if (*end == ' ') end++;
		char *body = strchr(end, 0x1f);
		char title[32] = "Phosphor";
		if (body) { size_t n = (size_t)(body - end) < sizeof(title) - 1 ? (size_t)(body - end) : sizeof(title) - 1; memcpy(title, end, n); title[n] = 0; body++; }
		else body = end;
		for (char *c = body; *c; c++) if (*c == 0x1f) *c = '\n';
		if (phosphor_osd_menu(title, body, (int)sel))
		{
			// MiSTer's menu still has the OSD: text mode ends, and the shell is told
			ingame_client = -1;
			ingame_end_text();
			rep_mask = 0;
			send_line(i, "{\"ok\":false,\"error\":\"MiSTer's menu has the OSD\"}");
			send_line(i, "{\"event\":\"ingame\",\"on\":false}");
			return;
		}
		send_line(i, "{\"ok\":true}");
	}
	else if (!strcmp(line, "ingame osd"))
	{
		// from Phosphor's in-game menu to MiSTer's own (the core's options): Phosphor's screen off, the OSD open
		if (ingame_client == i) { ingame_client = -1; rep_mask = 0; ingame_end_text(); if (!is_menu() && video_fb_state()) video_fb_enable(0); }
		// a press and, a moment later, its release: MiSTer's menu opens the OSD on the release, and it only
		// sees a key that changed (its 20 ms debounce), so both can't be set at once
		if (!is_menu() && !user_io_osd_is_visible()) { osd_direct = 1; f12_down = 0; menu_key_set(KEY_F12); osd_release = GetTimer(60); }
		send_line(i, "{\"ok\":true}");
	}
	else if (!strcmp(line, "ingame off"))
	{
		if (ingame_client == i) { ingame_client = -1; rep_mask = 0; ingame_end_text(); if (!is_menu() && video_fb_state()) video_fb_enable(0); }
		send_line(i, "{\"ok\":true}");
	}
	else if (!strcmp(line, "shot on") || !strcmp(line, "shot off"))
	{
		shot_on = !strcmp(line, "shot on");
		send_line(i, "{\"ok\":true}");
	}
	else if (!strcmp(line, "screenshot"))              // the apps' "take a screenshot" while a game runs
	{
		if (is_menu()) send_line(i, "{\"ok\":false,\"error\":\"no game is running\"}");
		else { shot_ours = 1; request_screenshot(NULL, 1); send_line(i, "{\"ok\":true}"); }
	}
	else if (!strncmp(line, "disc", 4) && (!line[4] || line[4] == ' ') && phosphor_disc_command(line, out, sizeof(out)))
		send_line(i, out);
	else if (!strcmp(line, "ra"))
	{
		char st[1200];
		achievements_phosphor_status(st, sizeof(st));
		send_line(i, st);
	}
	else if (!strcmp(line, "ra list"))
	{
		char *l = achievements_phosphor_list();
		send_line(i, l ? l : "{\"ok\":false,\"error\":\"out of memory\"}");
		free(l);
	}
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
	else if (!strcmp(line, "crt") || !strncmp(line, "crt ", 4)) crt_command(i, line[3] ? line + 4 : NULL);
	else if (!strncmp(line, "map_", 4)) map_command(i, line + 4);
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

// Controllers for the shell, the way MiSTer's own menu gets them: Main reads them, with the button mapping
// set up in MiSTer, and the shell gets the buttons as events {"event":"pad","b":"up","p":1|0|2} (2: held,
// repeated like MiSTer's menu does). Called from input.cpp while the Linux framebuffer is up.
static const char *pad_name(uint32_t m)
{
	switch (m)
	{
	case JOY_UP: return "up";
	case JOY_DOWN: return "down";
	case JOY_LEFT: return "left";
	case JOY_RIGHT: return "right";
	case JOY_A: return "a";
	case JOY_B: return "b";
	case JOY_X: return "x";
	case JOY_Y: return "y";
	case JOY_L: return "l";
	case JOY_R: return "r";
	case JOY_SELECT: return "select";
	case JOY_START: return "start";
	default: return NULL;
	}
}
static void pad_send(uint32_t m, int p)
{
	const char *b = pad_name(m);
	if (!b || owner < 0 || cl[owner].fd < 0) return;
	char ev[64];
	snprintf(ev, sizeof(ev), "{\"event\":\"pad\",\"b\":\"%s\",\"p\":%d}", b, p);
	send_line(owner, ev);
}
int phosphor_pad(uint32_t mask, int press)
{
	if (!press && mask == rep_mask) rep_mask = 0;   // whoever has the controller now: no more repeats
	if (script_state == 1) return 0;           // the script has the controllers (MiSTer's terminal keys)
	if (script_state == 2) { if (press) script_finish(); return 1; }
	if (owner < 0 || !(is_menu() || ingame_client == owner) || !pad_name(mask)) return 0;
	pad_send(mask, press ? 1 : 0);
	if (press && (mask & JOY_MOVE)) { rep_mask = mask; rep_next = GetTimer(320); }
	return 1;
}

void phosphor_poll(void)
{
	if (osd_release && CheckTimer(osd_release)) { osd_release = 0; menu_key_set(KEY_F12 | UPSTROKE); }   // "ingame osd"
	static int started;
	if (!started) { started = 1; busy_init(); }
	if (rep_mask && CheckTimer(rep_next))
	{
		if (owner < 0) rep_mask = 0;
		else { pad_send(rep_mask, 2); rep_next = GetTimer(85); }
	}
	if (!inited) ph_init();
	if (lfd < 0) return;
	script_poll();

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
	map_poll();
	phosphor_disc_poll(event_all);
	// MiSTer's menu (a long press) or Main itself took the screen back from Phosphor's in-game menu
	// MiSTer's menu (a long press) took the OSD from Phosphor's text in-game menu: that ends it
	if (ingame_text && ingame_client >= 0)
	{
		if (phosphor_osd_menu_shown()) text_shown_at = GetTimer(1000);
		else if (text_shown_at && CheckTimer(text_shown_at))
		{
			if (cl[ingame_client].fd >= 0) send_line(ingame_client, "{\"event\":\"ingame\",\"on\":false}");
			ingame_client = -1;
			ingame_text = 0;
			rep_mask = 0;
			text_shown_at = 0;
		}
	}
	else text_shown_at = 0;
	if (ingame_client >= 0 && !ingame_text && !is_menu() && !video_fb_state())
	{
		if (cl[ingame_client].fd >= 0) send_line(ingame_client, "{\"event\":\"ingame\",\"on\":false}");
		ingame_client = -1;
		rep_mask = 0;
	}

	// events
	char core[64], ev[256], name[128];
	snprintf(core, sizeof(core), "%s", user_io_get_core_name());
	if (strcmp(core, last_core))
	{
		snprintf(last_core, sizeof(last_core), "%s", core);
		ext_ver = -1;                       // another core (or the menu core again): ask again
		ingame_client = -1;                 // the in-game screen belongs to the game that's gone
		ingame_end_text();
		rep_mask = 0;
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
	if (owner >= 0 && is_menu() && !video_fb_state() && !script_state) menu_phosphor_screen(1);
	// black behind the menu while Phosphor has (or is about to have) the screen, MiSTer's static while its
	// own menu does. Sent again after a core change (a new menu core starts black).
	static int look = -1;
	int want = (owner < 0 && !hold_until) ? 1 : 0;
	if (ext_ver < 0) { look = -1; crt.applied = 0; }
	// Phosphor's CRT mode, set again on a menu core that just loaded (only Phosphor's has the native path)
	if (is_menu() && !crt.applied && ext_present()) { crt_load(); if (crt.on || crt.loaded) crt_apply(); }
	if (is_menu() && want != look && ext_present())
	{
		spi_uio_cmd_cont(PH_LOOK);
		spi_w((uint16_t)want);
		DisableIO();
		look = want;
	}
}

#define PH_SCRIPT "/media/fat/phosphor/phosphor.sh"

const char *phosphor_menu_core(void)
{
	static const char *p = "/media/fat/phosphor/cores/menu_phosphor.rbf";
	struct stat st;
	// a whole core is a few MB; anything else (a failed copy) is ignored and the stock one loads
	return (!stat(p, &st) && S_ISREG(st.st_mode) && st.st_size > 1000000) ? p : NULL;
}

int phosphor_hold_menu(void)
{
	if (!inited) ph_init();
	if (!hold_until) return 0;
	if (owner >= 0 || !is_menu() || CheckTimer(hold_until)) { hold_until = 0; return 0; }
	return 1;
}

// MiSTer.ini's main= for a core: 1 to stay in Phosphor's Main. The default (MiSTer) is Phosphor's Main
// here. So are the Mains whose work is merged into it, which Update All's options add sections for:
//   [A0CD-*] main=MiSTer_Physical-CD   the dock's CD drive (Main_MiSTer_Physical_Disc, merged)
//   [RA_*]   main=MiSTer_RA            RetroAchievements (odelot's Main_MiSTer, merged)
// Following those would leave Phosphor for the whole game (no in-game menu, no shell). Any other main=
// (3S-ARM, Quake, Duke3D: cores that really need their own Main) is still followed.
int phosphor_keep_main(const char *cfg_main)
{
	if (!cfg_main || !*cfg_main) return 1;
	const char *b = strrchr(cfg_main, '/');
	b = b ? b + 1 : cfg_main;
	static const char *const ours[] = { "MiSTer", "MiSTer_Physical-CD", "MiSTer_RA" };
	for (const char *o : ours) if (!strcasecmp(b, o)) return 1;
	return 0;
}

int phosphor_installed(void)
{
	return access(PH_SCRIPT, R_OK) == 0;
}

int phosphor_start(void)
{
	// the shell is running but gave the screen to Main's menu ("MiSTer menu" in Phosphor): ask it back
	if (owner < 0 && is_menu())
	{
		for (int i = 0; i < PH_MAX; i++) if (cl[i].fd >= 0 && cl[i].subscribed)
		{
			event_all("{\"event\":\"show\"}");
			return 1;
		}
	}
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
	// the menu key Main sends itself for "ingame osd": straight to the OSD (not a short press for the
	// in-game menu, which would wait for a release that never comes)
	if (osd_direct && key == KEY_F12) { if (c & UPSTROKE) osd_direct = 0; return c; }

	// MiSTer's own menu, after Phosphor gave it the screen: holding the menu button 2 s goes back to Phosphor
	static unsigned long menu_down;
	if (owner < 0 && is_menu() && !script_state && key == KEY_F12 && phosphor_installed())
	{
		if (!(c & UPSTROKE)) { if (!menu_down) menu_down = GetTimer(0); }
		else
		{
			int held = menu_down && GetTimer(0) - menu_down >= 2000;
			menu_down = 0;
			if (held && phosphor_start()) return 0;
		}
	}

	if (script_state == 2 && (c & UPSTROKE)) { script_finish(); return 0; }   // "press any key"
	if (script_state) return 0;                // the keyboard types into the script's terminal
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
		// any press, short or held, opens Phosphor's in-game menu; MiSTer's OSD opens only from its
		// "MiSTer menu (OSD)" entry (8BitDo pads power off when Home is held, so no hold-for-OSD)
		f12_down = 0;
		event_all("{\"event\":\"menu_button\"}");       // the in-game sheet
		return 0;
	}
	return c;
}
