// Phosphor: hybrid cores (gmcnaught's mister-hybrid-platform ports: Maldita Castilla and the other GameMaker,
// Solarus and Godot ports), run by Phosphor's Main. Part of phosphor-main (a fork of MiSTer-devel/Main_MiSTer),
// GPL-3.0 like Main. Copyright (C) 2026 f3bandit. GPL-3.0 (see LICENSE).
//
// A hybrid port runs its game engine on the ARM side and ships a launcher (launch.sh) plus an entry for it,
// games/<gamedir>/platform/hybrid.d/<CORENAME>.conf, read by its own main= binary (MiSTer_hybrid, an older
// Main_MiSTer). Phosphor's Main does that binary's job itself, written here from the platform's documented
// contract (its README and hybrid_registry.h): once the core is loaded and the FPGA is ready, the core's entry
// is looked up and its launcher started, detached, in its own session; a core with no entry is an ordinary
// core. The entry's keys:
//   launcher=   absolute path, required        log=       the launcher's output (optional)
//   noengine=   a flag file: don't start        osd_reset= the core's OSD Reset bit: restart the launcher
//   reset_clear= files or empty folders removed before that restart (up to 4)
// Where the entry is: next to main= when it names a MiSTer_hybrid ([Maldita Castilla] main=.../platform/
// MiSTer_hybrid, the port's "Cores menu" option); otherwise in any games/*/platform/hybrid.d, so the game
// also starts from Phosphor's library without that option.
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "hybrid.h"
#include "user_io.h"
#include "cfg.h"
#include "file_io.h"
#include "hardware.h"
#include "fpga_io.h"

#define HY_PATH 256
#define HY_CLEAR 4

static struct
{
	char launcher[HY_PATH], log[HY_PATH], noengine[HY_PATH];
	int reset_bit;
	int nclear;
	char clear[HY_CLEAR][HY_PATH];
} ent;

static int decided, armed, prev_bit;
static pid_t child = -1;
static int restarting;                  // 1: SIGTERM sent, 2: SIGKILL sent
static unsigned long restart_at;

static void hlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void hlog(const char *fmt, ...)
{
	char b[400];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	printf("Hybrid core: %s\n", b);
	if (ent.log[0])
	{
		FILE *f = fopen(ent.log, "a");
		if (f) { fprintf(f, "phosphor-main: %s\n", b); fclose(f); }
	}
}

// the core's name as a file name: letters, digits, _ . - and inner spaces, up to 64
static int name_ok(const char *n)
{
	size_t l = n ? strlen(n) : 0;
	if (!l || l > 64 || n[0] == ' ' || n[l - 1] == ' ') return 0;
	for (const char *c = n; *c; c++) if (!isalnum((unsigned char)*c) && !strchr("_.- ", *c)) return 0;
	return 1;
}

static void trim(char *s)
{
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
	char *b = s;
	while (isspace((unsigned char)*b)) b++;
	if (b != s) memmove(s, b, strlen(b) + 1);
}

// 1: read; 0: no such file; -1: there but not usable (the reason logged)
static int read_entry(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	memset(&ent, 0, sizeof(ent));
	ent.reset_bit = -1;
	char line[512];
	int bad = 0;
	while (fgets(line, sizeof(line), f))
	{
		trim(line);
		if (!line[0] || line[0] == '#') continue;
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		char *k = line, *v = eq + 1;
		trim(k);
		trim(v);
		if (strlen(v) >= HY_PATH) { bad = 1; continue; }
		if (!strcmp(k, "launcher")) strcpy(ent.launcher, v);
		else if (!strcmp(k, "log")) strcpy(ent.log, v);
		else if (!strcmp(k, "noengine")) strcpy(ent.noengine, v);
		else if (!strcmp(k, "osd_reset"))
		{
			char *end;
			long b = strtol(v, &end, 10);
			if (*end || b < 0 || b > 31) bad = 1; else ent.reset_bit = (int)b;
		}
		else if (!strcmp(k, "reset_clear"))
		{
			if (ent.nclear < HY_CLEAR && v[0] == '/') strcpy(ent.clear[ent.nclear++], v); else bad = 1;
		}
		// (profile= and anything newer: the launcher's business)
	}
	fclose(f);
	if (bad || ent.launcher[0] != '/' || access(ent.launcher, X_OK))
	{
		printf("Hybrid core: %s isn't usable (%s)\n", path, bad ? "a bad value" : "its launcher isn't there");
		return -1;
	}
	return 1;
}

static int find_entry(const char *core)
{
	char p[512];
	const char *m = cfg.main;
	const char *b = strrchr(m, '/');
	if (m[0] && !strcasecmp(b ? b + 1 : m, "MiSTer_hybrid"))
	{
		// next to the main= the port set
		char full[512];
		snprintf(full, sizeof(full), "%s", getFullPath(m));
		char *s = strrchr(full, '/');
		if (s)
		{
			*s = 0;
			snprintf(p, sizeof(p), "%s/hybrid.d/%s.conf", full, core);
			int r = read_entry(p);
			if (r) return r;
		}
	}
	glob_t g;
	snprintf(p, sizeof(p), "/media/fat/games/*/platform/hybrid.d/%s.conf", core);
	int r = 0;
	if (!glob(p, 0, NULL, &g))
	{
		for (size_t i = 0; i < g.gl_pathc && r <= 0; i++) r = read_entry(g.gl_pathv[i]);
		globfree(&g);
	}
	return r;
}

static void make_dirs_for(const char *file)
{
	char d[HY_PATH];
	snprintf(d, sizeof(d), "%s", file);
	char *s = strrchr(d, '/');
	if (!s || s == d) return;
	*s = 0;
	for (char *c = d + 1; *c; c++) if (*c == '/') { *c = 0; mkdir(d, 0755); *c = '/'; }
	mkdir(d, 0755);
}

static void spawn(void)
{
	if (ent.log[0]) make_dirs_for(ent.log);
	pid_t pid = fork();
	if (pid < 0) { hlog("couldn't start %s: %s", ent.launcher, strerror(errno)); return; }
	if (!pid)
	{
		setsid();                                      // its own session, out of Main's
		int nul = open("/dev/null", O_RDONLY);
		if (nul >= 0) { dup2(nul, 0); if (nul > 0) close(nul); }
		if (ent.log[0])
		{
			int lf = open(ent.log, O_WRONLY | O_CREAT | O_APPEND, 0644);
			if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); if (lf > 2) close(lf); }
		}
		cpu_set_t all;                                 // Main is on CPU 1: the launcher places itself
		CPU_ZERO(&all);
		CPU_SET(0, &all);
		CPU_SET(1, &all);
		sched_setaffinity(0, sizeof(all), &all);
		struct sigaction sa = {};
		sa.sa_handler = SIG_DFL;
		for (int s = 1; s < 32; s++) sigaction(s, &sa, NULL);
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		char *const argv[] = { ent.launcher, NULL };
		execv(ent.launcher, argv);
		_exit(127);
	}
	child = pid;
	hlog("core %s: started %s (pid %d)", user_io_get_core_name(), ent.launcher, (int)pid);
}

static void signal_group(int sig)
{
	if (child > 0 && kill(-child, sig)) kill(child, sig);
}

void hybrid_poll(void)
{
	if (!decided)
	{
		if (!is_fpga_ready(1)) return;               // (the engine started before that wedged the fabric)
		decided = 1;                                   // once per Main start: never two engines on one core
		if (is_menu()) return;
		const char *core = user_io_get_core_name();
		if (!name_ok(core) || find_entry(core) <= 0) return;
		struct stat st;
		if (ent.noengine[0] && !stat(ent.noengine, &st)) { hlog("%s is there: not starting %s", ent.noengine, ent.launcher); return; }
		spawn();
		if (child > 0 && ent.reset_bit >= 0)
		{
			armed = 1;
			char opt[8];
			snprintf(opt, sizeof(opt), "[%d]", ent.reset_bit);
			prev_bit = user_io_status_get(opt) & 1;
			hlog("the OSD's Reset (bit %d) restarts it", ent.reset_bit);
		}
		return;
	}
	if (child > 0)
	{
		int st;
		if (waitpid(child, &st, WNOHANG) == child)
		{
			hlog("launcher ended (%s %d)", WIFEXITED(st) ? "exit" : "signal", WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
			child = -1;
		}
	}
	if (!armed) return;

	// the OSD's Reset: its status bit's rising edge (the pulse is set and cleared within one HandleUI)
	char opt[8];
	snprintf(opt, sizeof(opt), "[%d]", ent.reset_bit);
	int bit = user_io_status_get(opt) & 1;
	int pressed = bit && !prev_bit;
	prev_bit = bit;
	if (!restarting)
	{
		if (!pressed) return;
		if (child > 0)
		{
			hlog("OSD Reset: stopping the launcher");
			signal_group(SIGTERM);
			restarting = 1;
			restart_at = GetTimer(3000);
			return;
		}
		restarting = 3;                                // nothing running: start it again now
	}
	if (restarting == 1 && child > 0)
	{
		if (!CheckTimer(restart_at)) return;
		hlog("OSD Reset: the launcher didn't stop, SIGKILL");
		signal_group(SIGKILL);
		restarting = 2;
		restart_at = GetTimer(2000);
		return;
	}
	if (restarting == 2 && child > 0 && !CheckTimer(restart_at)) return;
	for (int i = 0; i < ent.nclear; i++)
		if (unlink(ent.clear[i]) && (errno == EISDIR || errno == EPERM)) rmdir(ent.clear[i]);
	restarting = 0;
	hlog("OSD Reset: starting it again");
	spawn();
}

void hybrid_stop(void)
{
	if (child <= 0) return;
	hlog("stopping the launcher (another core)");
	signal_group(SIGTERM);
	for (int i = 0; i < 100; i++)                    // its own teardown: up to 1 s
	{
		if (waitpid(child, NULL, WNOHANG) == child) { child = -1; return; }
		usleep(10000);
	}
	signal_group(SIGKILL);
	waitpid(child, NULL, 0);
	child = -1;
}

int hybrid_main(const char *main_name)
{
	if (!main_name || !*main_name) return 0;
	const char *b = strrchr(main_name, '/');
	return !strcasecmp(b ? b + 1 : main_name, "MiSTer_hybrid");
}
