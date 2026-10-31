// Phosphor: game ports (Quake, Duke Nukem 3D) run by Phosphor's Main. See gameport.h.
// Part of phosphor-main (a fork of MiSTer-devel/Main_MiSTer), GPL-3.0 like Main.
// Based on the MiSTer_Quake and MiSTer_Duke3D wrappers by neofreno (GPL-2.0-or-later).
// Copyright (C) 2026 f3bandit. GPL-3.0 (see LICENSE).
//
// What the ports' own Mains did, done here instead:
//   - the game's data checked first; if it's missing, Phosphor says which file and where (no black screen)
//   - the core's picture buffer cleared, so the last frame of the previous run isn't shown while it starts
//   - the game started on CPU 0 (Main stays on CPU 1) with the ARM clock at its top speed while it runs
//   - the controllers handed to the game through a small shared-memory block it reads (/dev/shm/...)
//   - the core's OSD options passed on (Music off, FPS overlay) and its Restart honoured
//   - quitting the game goes back to Phosphor; a crash restarts it, three in a minute stop it with a note
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <sched.h>
#include <time.h>
#include <stdarg.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <linux/input.h>

#include "gameport.h"
#include "user_io.h"
#include "menu.h"
#include "fpga_io.h"
#include "shmem.h"
#include "hardware.h"
#include "phosphor.h"

uint32_t build_joy_mask(int player);   // input.cpp: the core's buttons as MiSTer maps them

#define NOTE_PATH "/tmp/phosphor-port-note"

// ---------- the ports ----------

struct port_t
{
	const char *title;            // as Phosphor shows it
	const char *cores[3];         // the core's name (its CONF_STR's first field; older bitstreams differ)
	const char *mains[2];         // its own Main's name (main= in MiSTer.ini)
	const char *dir;              // its folder under /media/fat
	const char *data[2];          // the game data it needs (one of them), relative to dir
	const char *data_dir;         // where that goes, relative to dir ("" = dir itself)
	const char *data_hint;        // what else is worth knowing
	const char *engine;           // the game, relative to dir
	const char *rbf;              // its core, if the path isn't known
	const char *env;              // its environment variables' prefix (QUAKE_HOME, DN3D_HOME...)
	const char *shm;              // the controllers' shared-memory file
	const char *proc;             // the game's process name
	int quake_buttons;            // the core's J1 has 6 buttons (Quake): Select and Start at bits 8 and 9
	int duke_cfg;                 // duke3d.cfg kept at the core's 320x200
	const char *credit;           // who made the port, and what it's built on (shown by Phosphor)
};

static const port_t ports[] = {
	{ "Quake", { "MiSTer_Quake", "QUAKE", NULL }, { "MiSTer_Quake", NULL }, "games/quake",
	  { "id1/PAK0.PAK", NULL }, "id1", "PAK1.PAK (the full game) is optional, next to it.",
	  "bin/quake-mister", "_Other/Quake.rbf", "QUAKE", "/dev/shm/quake-mister-joy", "quake-mister", 1, 0,
	  "MiSTer port by neofreno (github.com/neofreno/mister_quake), on TyrQuake and id Software's GPL Quake source, after kimchiman52's ARM ports" },
	{ "Duke Nukem 3D", { "duke3d", "DUKE3D", NULL }, { "Mister_duke3d", NULL }, "games/DUKE3D",
	  { "duke3d.grp", NULL }, "", "From your own copy of the game (the shareware DUKE3D.GRP works too).",
	  "bin/duke3d-mister", "_Other/DUKE3D.rbf", "DN3D", "/dev/shm/duke3d-mister-joy", "duke3d-mister", 0, 1,
	  "MiSTer port by neofreno (github.com/neofreno/mister_duke3d), on 3D Realms' GPL Duke Nukem 3D source, after kimchiman52's ARM ports" },
};
#define NPORTS ((int)(sizeof(ports) / sizeof(ports[0])))

int gameport_main_merged(const char *main_name)
{
	if (!main_name || !*main_name) return 0;
	const char *b = strrchr(main_name, '/');
	b = b ? b + 1 : main_name;
	for (int i = 0; i < NPORTS; i++)
		for (int m = 0; m < 2 && ports[i].mains[m]; m++)
			if (!strcasecmp(b, ports[i].mains[m])) return 1;
	return 0;
}

// ---------- state ----------

static const port_t *port;
static char rbf[PATH_MAX];
static char base[PATH_MAX];       // /media/fat/<dir>
static FILE *logf;
static pid_t child = -1;
static unsigned long start_at;    // the game starts once the core has settled (GetTimer)
static int stopping;
static int restart_prev;
static time_t fails[3];
static int nfails;
static char cpu_max_orig[32];
static unsigned long rescan_at;

#define CPU_MAX_NODE "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq"
#define CPU_GOV_NODE "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
#define CPU_GOV_SAVE "/tmp/phosphor-port-gov"

static void plog(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	printf("Game port: %s\n", buf);
	if (logf)
	{
		time_t now = time(NULL);
		char ts[32];
		strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
		fprintf(logf, "[%s] %s\n", ts, buf);
		fflush(logf);
	}
}

static int is_file(const char *p)
{
	struct stat st;
	return !stat(p, &st) && S_ISREG(st.st_mode);
}

// ---------- the note Phosphor shows ----------

static void json_str(FILE *f, const char *key, const char *v, int comma)
{
	fprintf(f, "\"%s\":\"", key);
	for (const unsigned char *c = (const unsigned char *)(v ? v : ""); *c; c++)
	{
		if (*c == '"' || *c == '\\') fprintf(f, "\\%c", *c);
		else if (*c < 0x20) fputc(' ', f);
		else fputc(*c, f);
	}
	fprintf(f, "\"%s", comma ? "," : "");
}

// what's in the data folder now, for the note ("nothing yet" when empty or not there)
static void found_list(char *out, size_t n)
{
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s%s%s", base, *port->data_dir ? "/" : "", port->data_dir);
	out[0] = 0;
	DIR *d = opendir(dir);
	int k = 0, more = 0;
	if (d)
	{
		struct dirent *de;
		while ((de = readdir(d)))
		{
			if (de->d_name[0] == '.') continue;
			if (!strcasecmp(de->d_name, "logs") || !strcasecmp(de->d_name, "bin") || !strcasecmp(de->d_name, "libw") ||
				!strcasecmp(de->d_name, "lib")) continue;
			if (k == 4) { more++; continue; }
			size_t l = strlen(out);
			snprintf(out + l, n - l, "%s%s", k ? ", " : "", de->d_name);
			k++;
		}
		closedir(d);
	}
	if (!k) snprintf(out, n, "nothing yet");
	else if (more) { size_t l = strlen(out); snprintf(out + l, n - l, " and %d more", more); }
}

// why: "data" (the game data isn't there), "engine" (the game itself isn't installed), "stopped" (it kept
// quitting). Then back to Phosphor, which shows it; without Phosphor, MiSTer's OSD says it.
static void note_and_leave(const char *why)
{
	gameport_stop();                             // nothing of the game left running, the clock put back
	char need[PATH_MAX], found[256];
	snprintf(need, sizeof(need), "%s/%s", port->dir, port->data[0]);
	found_list(found, sizeof(found));

	FILE *f = fopen(NOTE_PATH ".tmp", "w");
	if (f)
	{
		fputc('{', f);
		json_str(f, "why", why, 1);
		json_str(f, "title", port->title, 1);
		const char *file = strrchr(port->data[0], '/');
		json_str(f, "file", file ? file + 1 : port->data[0], 1);
		char d[PATH_MAX];
		snprintf(d, sizeof(d), "%s%s%s", port->dir, *port->data_dir ? "/" : "", port->data_dir);
		json_str(f, "dir", d, 1);
		json_str(f, "path", need, 1);
		json_str(f, "found", found, 1);
		json_str(f, "hint", port->data_hint, 1);
		char lg[PATH_MAX];
		snprintf(lg, sizeof(lg), "%s/logs/last-run.log", port->dir);
		json_str(f, "log", lg, 1);
		json_str(f, "credit", port->credit, 1);
		json_str(f, "rbf", rbf, 0);
		fputs("}\n", f);
		fclose(f);
		rename(NOTE_PATH ".tmp", NOTE_PATH);
	}
	plog("%s: %s", why, need);

	if (!phosphor_installed())
	{
		char msg[512];
		if (!strcmp(why, "data")) snprintf(msg, sizeof(msg), "%s needs its game data:\n%s", port->title, need);
		else if (!strcmp(why, "engine")) snprintf(msg, sizeof(msg), "%s isn't installed:\n%s/%s missing", port->title, port->dir, port->engine);
		else snprintf(msg, sizeof(msg), "%s keeps stopping.\nSee %s/logs/last-run.log", port->title, port->dir);
		Info(msg, 10000);
		return;
	}
	fpga_load_rbf("menu.rbf");
}

// ---------- the ARM clock ----------

static void wr(const char *node, const char *v)
{
	int fd = open(node, O_WRONLY | O_CLOEXEC);
	if (fd < 0) return;
	if (write(fd, v, strlen(v)) < 0) {}
	close(fd);
}

static void rd(const char *node, char *buf, size_t n)
{
	buf[0] = 0;
	int fd = open(node, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return;
	ssize_t r = read(fd, buf, n - 1);
	close(fd);
	if (r > 0) { buf[r] = 0; char *nl = strchr(buf, '\n'); if (nl) *nl = 0; }
	else buf[0] = 0;
}

static void clock_up(void)
{
	rd(CPU_MAX_NODE, cpu_max_orig, sizeof(cpu_max_orig));
	char gov[32];
	rd(CPU_GOV_NODE, gov, sizeof(gov));
	FILE *f = fopen(CPU_GOV_SAVE, "w");        // kept across Main's restart, which loads the next core
	if (f) { fprintf(f, "%s %s\n", gov[0] ? gov : "ondemand", cpu_max_orig); fclose(f); }
	wr(CPU_GOV_NODE, "performance");
	wr(CPU_MAX_NODE, "1000000");
}

static void clock_restore(void)
{
	char gov[32] = {}, mx[32] = {};
	FILE *f = fopen(CPU_GOV_SAVE, "r");
	if (!f) return;
	if (fscanf(f, "%31s %31s", gov, mx) >= 1)
	{
		wr(CPU_GOV_NODE, gov);
		if (mx[0]) wr(CPU_MAX_NODE, mx);
	}
	fclose(f);
	unlink(CPU_GOV_SAVE);
}

// ---------- controllers ----------

#define NPAD 2
enum {
	QJ_RIGHT = 1 << 0, QJ_LEFT = 1 << 1, QJ_DOWN = 1 << 2, QJ_UP = 1 << 3,
	QJ_A = 1 << 4, QJ_B = 1 << 5, QJ_X = 1 << 6, QJ_Y = 1 << 7,
	QJ_L = 1 << 8, QJ_R = 1 << 9, QJ_SELECT = 1 << 10, QJ_START = 1 << 11,
};
#define QJ_DIRS (QJ_RIGHT | QJ_LEFT | QJ_DOWN | QJ_UP)
#define QJ_ALL 0xFFFu

struct joy_shm_t                      // the games' layout (mister_joy_shm.h in the ports)
{
	uint32_t magic;                   // "QKAK"
	uint32_t version;                 // 1
	uint32_t joy_mask[NPAD];
	int8_t left_x[NPAD], left_y[NPAD];
	int8_t right_x[NPAD], right_y[NPAD];
};

static joy_shm_t *shm;
static int pad_fd[NPAD] = { -1, -1 };
static int pad_rz[NPAD];              // the right stick is Z/RZ (RX/RY are triggers on this pad)

static int bit(const uint8_t *b, int c) { return (b[c / 8] >> (c % 8)) & 1; }

static void shm_open_(void)
{
	int fd = open(port->shm, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) { plog("controllers: can't create %s: %s", port->shm, strerror(errno)); return; }
	if (ftruncate(fd, sizeof(joy_shm_t)) < 0) { close(fd); return; }
	void *p = mmap(NULL, sizeof(joy_shm_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (p == MAP_FAILED) return;
	shm = (joy_shm_t *)p;
	memset(shm, 0, sizeof(*shm));
	shm->magic = 0x514B414Bu;
	shm->version = 1;
}

static void shm_close(void)
{
	if (shm) { munmap(shm, sizeof(joy_shm_t)); shm = NULL; }
	if (port) unlink(port->shm);
}

static int axis_at_end(int fd, int code)
{
	struct input_absinfo a = {};
	if (ioctl(fd, EVIOCGABS(code), &a) < 0 || a.maximum == a.minimum) return 0;
	int r = a.maximum - a.minimum;
	return a.value <= a.minimum + r / 8 || a.value >= a.maximum - r / 8;
}

static int pad_open(const char *path, int *rz)
{
	int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) return -1;
	uint8_t ev[EV_MAX / 8 + 1] = {}, keys[KEY_MAX / 8 + 1] = {}, abs_[ABS_MAX / 8 + 1] = {};
	if (ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev) < 0 || !bit(ev, EV_KEY) ||
		ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0) { close(fd); return -1; }
	if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_)), abs_) < 0) memset(abs_, 0, sizeof(abs_));
	int face = bit(keys, BTN_SOUTH) || bit(keys, BTN_EAST) || bit(keys, BTN_NORTH) || bit(keys, BTN_WEST);
	int dpad = bit(keys, BTN_DPAD_UP) || bit(keys, BTN_DPAD_DOWN) || bit(abs_, ABS_HAT0X) || bit(abs_, ABS_HAT0Y);
	if (!face && !dpad) { close(fd); return -1; }
	// the right stick: RX/RY, unless those rest at an end (triggers) and Z/RZ don't
	*rz = 0;
	int has_r = bit(abs_, ABS_RX) && bit(abs_, ABS_RY), has_z = bit(abs_, ABS_Z) && bit(abs_, ABS_RZ);
	if (has_z && (!has_r || (axis_at_end(fd, ABS_RX) && !axis_at_end(fd, ABS_Z)))) *rz = 1;
	return fd;
}

static void pads_scan(void)
{
	for (int i = 0; i < NPAD; i++) if (pad_fd[i] >= 0) { close(pad_fd[i]); pad_fd[i] = -1; }
	DIR *d = opendir("/dev/input");
	if (!d) return;
	struct dirent *de;
	char names[64][16];
	int nn = 0;
	while ((de = readdir(d)) && nn < 64)
		if (!strncmp(de->d_name, "event", 5)) snprintf(names[nn++], sizeof(names[0]), "%s", de->d_name);
	closedir(d);
	// in eventN order, so player 1 stays player 1
	qsort(names, nn, sizeof(names[0]), [](const void *a, const void *b) {
		return atoi((const char *)a + 5) - atoi((const char *)b + 5);
	});
	int found = 0;
	for (int i = 0; i < nn && found < NPAD; i++)
	{
		char p[64];
		snprintf(p, sizeof(p), "/dev/input/%s", names[i]);
		int rz = 0, fd = pad_open(p, &rz);
		if (fd < 0) continue;
		pad_fd[found] = fd;
		pad_rz[found] = rz;
		plog("player %d: %s%s", found + 1, p, rz ? " (right stick on Z/RZ)" : "");
		found++;
	}
}

static int8_t axis(int fd, int code)
{
	struct input_absinfo a = {};
	if (ioctl(fd, EVIOCGABS(code), &a) < 0 || a.maximum == a.minimum) return 0;
	long r = (long)a.maximum - a.minimum;
	long v = ((long)(a.value - a.minimum) * 254) / r - 127;
	return (int8_t)(v < -127 ? -127 : v > 127 ? 127 : v);
}

// the pad as the game expects it (SNES positions: east A, south B, north X, west Y), read from the kernel's
// state: works while Main holds the pads (EVIOCGRAB doesn't hide it)
static uint32_t pad_mask(int fd)
{
	uint32_t m = 0;
	uint8_t keys[KEY_MAX / 8 + 1] = {};
	if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) >= 0)
	{
		static const struct { int code; uint32_t b; } map[] = {
			{ BTN_EAST, QJ_A }, { BTN_SOUTH, QJ_B }, { BTN_NORTH, QJ_X }, { BTN_WEST, QJ_Y },
			{ BTN_TL, QJ_L }, { BTN_TR, QJ_R }, { BTN_SELECT, QJ_SELECT }, { BTN_START, QJ_START },
			{ BTN_DPAD_UP, QJ_UP }, { BTN_DPAD_DOWN, QJ_DOWN }, { BTN_DPAD_LEFT, QJ_LEFT }, { BTN_DPAD_RIGHT, QJ_RIGHT },
		};
		for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) if (bit(keys, map[i].code)) m |= map[i].b;
	}
	struct input_absinfo hx = {}, hy = {};
	if (!ioctl(fd, EVIOCGABS(ABS_HAT0X), &hx)) { if (hx.value < 0) m |= QJ_LEFT; if (hx.value > 0) m |= QJ_RIGHT; }
	if (!ioctl(fd, EVIOCGABS(ABS_HAT0Y), &hy)) { if (hy.value < 0) m |= QJ_UP; if (hy.value > 0) m |= QJ_DOWN; }
	int8_t x = axis(fd, ABS_X), y = axis(fd, ABS_Y);
	if (x < -64) m |= QJ_LEFT;
	if (x > 64) m |= QJ_RIGHT;
	if (y < -64) m |= QJ_UP;
	if (y > 64) m |= QJ_DOWN;
	return m;
}

// the core's buttons as MiSTer maps them (the user's own mapping), in the games' bit order
static uint32_t mapped_mask(int p)
{
	uint32_t m = build_joy_mask(p);
	if (!port->quake_buttons) return m & QJ_ALL;
	// Quake's J1: Fire, Jump, Next Weapon, Prev Weapon, Select, Start (bits 4..9)
	uint32_t o = m & (QJ_DIRS | QJ_A | QJ_B | QJ_X | QJ_Y);
	if (m & (1 << 8)) o |= QJ_SELECT;
	if (m & (1 << 9)) o |= QJ_START;
	return o;
}

static void pads_feed(void)
{
	if (!shm) return;
	// MiSTer's OSD or Phosphor's in-game menu open: the game gets nothing (the pads drive the menu)
	int held = menu_present() || phosphor_ingame_shown();
	for (int p = 0; p < NPAD; p++)
	{
		uint32_t m = 0;
		int8_t lx = 0, ly = 0, rx = 0, ry = 0;
		if (!held)
		{
			uint32_t mapped = mapped_mask(p);
			uint32_t direct = pad_fd[p] >= 0 ? pad_mask(pad_fd[p]) : 0;
			// directions from both; buttons from MiSTer's mapping when it has any, so one press is one button
			m = ((mapped | direct) & QJ_DIRS) | ((mapped & ~QJ_DIRS) ? (mapped & ~QJ_DIRS) : (direct & ~QJ_DIRS));
			if (pad_fd[p] >= 0)
			{
				lx = axis(pad_fd[p], ABS_X);
				ly = axis(pad_fd[p], ABS_Y);
				rx = axis(pad_fd[p], pad_rz[p] ? ABS_Z : ABS_RX);
				ry = axis(pad_fd[p], pad_rz[p] ? ABS_RZ : ABS_RY);
			}
		}
		shm->joy_mask[p] = m;
		shm->left_x[p] = lx;
		shm->left_y[p] = ly;
		shm->right_x[p] = rx;
		shm->right_y[p] = ry;
	}
}

// ---------- Duke's config: the core shows 320x200 ----------

static int key_is(const char *line, const char *key)
{
	while (*line == ' ' || *line == '\t') line++;
	size_t k = strlen(key);
	if (strncasecmp(line, key, k)) return 0;
	char c = line[k];
	return c == ' ' || c == '\t' || c == '=' || c == '\r' || c == '\n' || !c;
}

static void duke_cfg(void)
{
	static const char *const screen[][2] = { { "ScreenWidth", "320" }, { "ScreenHeight", "200" }, { "ScreenMode", "1" }, { "ScreenBPP", "8" } };
	char cfg[PATH_MAX], tmp[PATH_MAX + 8];
	snprintf(cfg, sizeof(cfg), "%s/duke3d.cfg", base);
	snprintf(tmp, sizeof(tmp), "%s.tmp", cfg);
	FILE *in = fopen(cfg, "r");
	FILE *out = fopen(tmp, "w");
	if (!out) { if (in) fclose(in); return; }
	int in_screen = 0, in_setup = 0, saw_screen = 0, saw_setup = 0, wrote[4] = {}, wrote_force = 0;
	char line[1024];
	auto close_screen = [&]() {
		for (int i = 0; i < 4; i++) if (!wrote[i]) fprintf(out, "%s = %s\n", screen[i][0], screen[i][1]);
		in_screen = 0;
	};
	while (in && fgets(line, sizeof(line), in))
	{
		if (line[0] == '[')
		{
			if (in_screen) close_screen();
			if (in_setup) { if (!wrote_force) fprintf(out, "ForceSetup = 0\n"); in_setup = 0; }
			if (!strncasecmp(line, "[Screen Setup]", 14)) saw_screen = in_screen = 1;
			else if (!strncasecmp(line, "[Setup]", 7)) saw_setup = in_setup = 1;
			fputs(line, out);
			continue;
		}
		int done = 0;
		if (in_screen)
			for (int i = 0; i < 4 && !done; i++)
				if (key_is(line, screen[i][0])) { fprintf(out, "%s = %s\n", screen[i][0], screen[i][1]); wrote[i] = done = 1; }
		if (in_setup && !done && key_is(line, "ForceSetup")) { fprintf(out, "ForceSetup = 0\n"); wrote_force = done = 1; }
		if (!done) fputs(line, out);
	}
	if (in_screen) close_screen();
	if (in_setup && !wrote_force) fprintf(out, "ForceSetup = 0\n");
	if (!saw_screen) { fprintf(out, "\n[Screen Setup]\n"); for (int i = 0; i < 4; i++) fprintf(out, "%s = %s\n", screen[i][0], screen[i][1]); }
	if (!saw_setup) fprintf(out, "\n[Setup]\nForceSetup = 0\n");
	if (in) fclose(in);
	if (fclose(out) || rename(tmp, cfg)) unlink(tmp);
}

// ---------- the game ----------

static void clear_picture(void)
{
	// the core reads its picture from 0x3A000000; CTRL = 0 makes it wait (black) for the game's first frame
	void *p = shmem_map(0x3A000000, 0x1000);
	if (!p) return;
	*(volatile uint32_t *)p = 0;
	__sync_synchronize();
	shmem_unmap(p, 0x1000);
}

static int launch(void)
{
	char engine[PATH_MAX], lastrun[PATH_MAX];
	snprintf(engine, sizeof(engine), "%s/%s", base, port->engine);
	snprintf(lastrun, sizeof(lastrun), "%s/logs/last-run.log", base);
	int music_off = user_io_status_get("[10]") != 0;
	uint32_t fps = user_io_status_get("[12:11]");

	int pipefd[2];
	if (pipe2(pipefd, O_CLOEXEC) < 0) return -1;
	pid_t pid = fork();
	if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }
	if (!pid)
	{
		close(pipefd[0]);
		prctl(PR_SET_PDEATHSIG, SIGKILL);       // never left running if Main goes
		setsid();
		cpu_set_t set;
		CPU_ZERO(&set);
		CPU_SET(0, &set);
		sched_setaffinity(0, sizeof(set), &set);
		struct sigaction sa = {};
		sa.sa_handler = SIG_DFL;
		for (int s = 1; s < 32; s++) sigaction(s, &sa, NULL);
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);

		int nul = open("/dev/null", O_RDONLY);
		if (nul >= 0) { dup2(nul, 0); close(nul); }
		int lf = open(lastrun, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); if (lf > 2) close(lf); }

		char k[64], v[PATH_MAX];
		setenv("HOME", base, 1);
		snprintf(k, sizeof(k), "%s_HOME", port->env); setenv(k, base, 1);
		snprintf(k, sizeof(k), "%s_BASEDIR", port->env); setenv(k, base, 1);
		snprintf(k, sizeof(k), "%s_NATIVE_VIDEO", port->env); setenv(k, "1", 1);
		snprintf(k, sizeof(k), "%s_FPS_OVERLAY", port->env); setenv(k, fps == 1 ? "1" : fps == 2 ? "2" : "0", 1);
		if (shm) { snprintf(k, sizeof(k), "%s_JOY_SHM", port->env); setenv(k, port->shm, 1); }
		snprintf(k, sizeof(k), "%s_RBF_PATH", port->env); setenv(k, rbf, 1);
		setenv("SDL_VIDEODRIVER", "dummy", 1);
		setenv("SDL_VIDEO_DRIVER", "dummy", 1);
		setenv("SDL_RENDER_DRIVER", "software", 1);
		setenv("SDL_AUDIODRIVER", "alsa", 1);
		setenv("ALSA_CARD", "default", 1);
		setenv("AUDIODEV", "default", 1);
		prctl(PR_SET_NAME, port->proc, 0, 0, 0);
		if (chdir(base)) {}

		const char *argv[5] = { engine, NULL, NULL, NULL, NULL };
		int a = 1;
		if (port->duke_cfg) { snprintf(v, sizeof(v), "-j%s", base); argv[a++] = v; }
		else { argv[a++] = "-basedir"; argv[a++] = base; }
		if (music_off) argv[a++] = "-nocdaudio";
		execv(engine, (char *const *)argv);
		int e = errno;
		if (write(pipefd[1], &e, sizeof(e)) < 0) {}
		_exit(127);
	}
	close(pipefd[1]);
	int e = 0;
	ssize_t r = read(pipefd[0], &e, sizeof(e));
	close(pipefd[0]);
	if (r > 0)
	{
		plog("couldn't start %s: %s", engine, strerror(e));
		waitpid(pid, NULL, 0);
		return -1;
	}
	plog("started %s (pid %d)%s", engine, (int)pid, music_off ? ", music off" : "");
	child = pid;
	return 0;
}

static void kill_child(void)
{
	if (child <= 0) return;
	kill(-child, SIGTERM);
	kill(child, SIGTERM);
	for (int i = 0; i < 50; i++)                 // up to 0.5 s to quit, then it's stopped
	{
		if (waitpid(child, NULL, WNOHANG) == child) { child = -1; return; }
		usleep(10000);
	}
	kill(-child, SIGKILL);
	kill(child, SIGKILL);
	waitpid(child, NULL, 0);
	child = -1;
}

int gameport_begin(const char *rbf_path)
{
	clock_restore();                             // a game port before this core left the ARM clock up
	const char *name = user_io_get_core_name(1);
	const char *name2 = user_io_get_core_name();
	for (int i = 0; i < NPORTS && !port; i++)
		for (int c = 0; c < 3 && ports[i].cores[c]; c++)
			if ((name && !strcasecmp(name, ports[i].cores[c])) || (name2 && !strcasecmp(name2, ports[i].cores[c]))) { port = &ports[i]; break; }
	if (!port) return 0;

	snprintf(base, sizeof(base), "/media/fat/%s", port->dir);
	if (rbf_path && *rbf_path) snprintf(rbf, sizeof(rbf), "%s", rbf_path);
	else snprintf(rbf, sizeof(rbf), "/media/fat/%s", port->rbf);
	char logs[PATH_MAX + 8], lp[PATH_MAX + 32];
	snprintf(logs, sizeof(logs), "%s/logs", base);
	mkdir(base, 0755);
	mkdir(logs, 0755);
	snprintf(lp, sizeof(lp), "%s/phosphor-port.log", logs);
	logf = fopen(lp, "w");
	plog("%s (core %s, %s)", port->title, name2 ? name2 : "?", rbf);
	plog("%s", port->credit);
	unlink(NOTE_PATH);

	char p[PATH_MAX];
	snprintf(p, sizeof(p), "%s/%s", base, port->engine);
	if (!is_file(p)) { note_and_leave("engine"); return 1; }
	int have = 0;
	for (int d = 0; d < 2 && port->data[d] && !have; d++)
	{
		snprintf(p, sizeof(p), "%s/%s", base, port->data[d]);
		have = is_file(p);
	}
	if (!have) { note_and_leave("data"); return 1; }
	if (port->duke_cfg) duke_cfg();

	clear_picture();
	clock_up();
	shm_open_();
	pads_scan();
	rescan_at = GetTimer(1000);
	start_at = GetTimer(2000);                   // the core settles first (as the ports' Mains waited)
	return 1;
}

int gameport_running(void) { return port && (child > 0 || start_at); }

void gameport_poll(void)
{
	if (!port || stopping) return;
	if (start_at)
	{
		if (!CheckTimer(start_at)) return;
		start_at = 0;
		if (launch() < 0) { note_and_leave("engine"); return; }
	}
	if (child <= 0) return;

	// the pads to the game at 500 Hz (it reads them once a frame): reading them on every pass of Main's
	// loop kept a core busy with ioctls
	static unsigned long feed_at;
	if (CheckTimer(feed_at)) { feed_at = GetTimer(2); pads_feed(); }
	if (CheckTimer(rescan_at))
	{
		rescan_at = GetTimer(1000);
		static unsigned long seen;
		struct stat st;
		unsigned long now = !stat("/dev/input", &st) ? (unsigned long)st.st_mtime : 0;
		if (now != seen) { seen = now; pads_scan(); }   // a pad plugged in or gone since
	}

	// the core's Restart (T[22]): the trigger, or the status bit's rising edge if the pulse was missed
	uint32_t trig = user_io_status_get("[22]") & 1;
	if (trig && !restart_prev)
	{
		plog("Restart from the OSD");
		kill_child();
		restart_prev = trig;
		if (launch() < 0) note_and_leave("engine");
		return;
	}
	restart_prev = trig;

	int st = 0;
	pid_t r = waitpid(child, &st, WNOHANG);
	if (r != child) return;
	child = -1;
	if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
	{
		plog("the game quit: back to Phosphor");
		gameport_stop();
		if (phosphor_installed()) fpga_load_rbf("menu.rbf");
		return;
	}
	plog("the game stopped (%s %d)", WIFEXITED(st) ? "exit" : "signal", WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
	time_t now = time(NULL);
	if (nfails == 3) { memmove(fails, fails + 1, sizeof(fails[0]) * 2); nfails = 2; }
	fails[nfails++] = now;
	if (nfails == 3 && now - fails[0] < 60)
	{
		gameport_stop();
		note_and_leave("stopped");
		return;
	}
	if (launch() < 0) note_and_leave("engine");
}

void gameport_stop(void)
{
	if (!port) { clock_restore(); return; }     // (a port's Main that ran before this one left the clock up)
	stopping = 1;
	start_at = 0;
	kill_child();
	for (int i = 0; i < NPAD; i++) if (pad_fd[i] >= 0) { close(pad_fd[i]); pad_fd[i] = -1; }
	shm_close();
	clock_restore();
	if (logf) { plog("stopped"); fclose(logf); logf = NULL; }
	stopping = 0;
}
