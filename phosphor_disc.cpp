// Phosphor: the dock's CD/DVD drive for Phosphor's API (phosphor.cpp). Part of phosphor-main, GPL-3.0.
//
// What's in the drive, read with the physical disc code merged from Main_MiSTer_Physical_Disc
// (Anime0t4ku, bonna79; support/physical_disc). Only in the menu core, only while no disc game runs:
//   the tray state is polled once a second (CDROM_DRIVE_STATUS: never spins or reads the disc);
//   a newly ready disc is read once, on a thread, so Main's loop (and the controllers) never wait
//   for the drive: its system, disc code (serial), label and region.
// Upstream's Auto Disc Discovery (physical_disc_launch_menu_tick) stays off and leaves the drive
// alone while this reads it (phosphor_disc_busy). Phosphor decides what a disc does: phosphord
// starts the matching core with an .mgl under an A0CD-* set name, the way upstream's do.
//
//   disc            {"ok":true,"drive":true,"state":"none|empty|reading|ready|unreadable",
//                    "type":"psx|saturn|megacd|pcecd|neogeocd|3do|cdi|mdplus|msu1|audio|dvd|unknown",
//                    "system":"PlayStation","serial":"SCUS-94300","label":"...","region":"US",
//                    "tracks":N,"playable":true,"mgl":"PSX"}
//   disc eject      opens the tray (also when no disc is in)
//   event           {"event":"disc",...the same...} to subscribers when it changes
#include "phosphor_disc.h"
#include "user_io.h"
#include "hardware.h"
#include "cd.h"
#include "support/physical_disc/physical_disc.h"
#include <fcntl.h>
#include <linux/cdrom.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct {
	int drive;              // a drive was found
	char state[12];         // none (no drive) empty reading ready unreadable
	physical_disc_disc_t type;
	int dvd;
	int tracks;
	char serial[32], label[40], region[4];
} disc_info_t;

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static disc_info_t cur = { 0, "none", PHYSICAL_DISC_DISC_NONE, 0, 0, "", "", "" };
static pthread_t worker;
static volatile int working;      // the worker has the drive
static int worker_joinable;
static int identified;            // the disc in the tray now has been read (until the tray empties)
static unsigned long next_poll;
static char last_sent[512];

int phosphor_disc_busy(void) { return working; }

static const char *type_key(physical_disc_disc_t t, int dvd)
{
	if (dvd) return "dvd";
	switch (t)
	{
	case PHYSICAL_DISC_DISC_PSX:    return "psx";
	case PHYSICAL_DISC_DISC_SATURN: return "saturn";
	case PHYSICAL_DISC_DISC_MEGACD: return "megacd";
	case PHYSICAL_DISC_DISC_PCECD:  return "pcecd";
	case PHYSICAL_DISC_DISC_NEOGEO: return "neogeocd";
	case PHYSICAL_DISC_DISC_3DO:    return "3do";
	case PHYSICAL_DISC_DISC_CDI:    return "cdi";
	case PHYSICAL_DISC_DISC_MDPLUS: return "mdplus";
	case PHYSICAL_DISC_DISC_SNES:   return "msu1";
	case PHYSICAL_DISC_DISC_AUDIO:  return "audio";
	case PHYSICAL_DISC_DISC_NONE:   return "";
	default:                        return "unknown";
	}
}

// the .mgl phosphord uses (the same names as upstream's "_Physical Disc Cores"); "" not playable
static const char *mgl_for(physical_disc_disc_t t, int dvd)
{
	if (dvd) return "";
	switch (t)
	{
	case PHYSICAL_DISC_DISC_PSX:    return "PSX";
	case PHYSICAL_DISC_DISC_SATURN: return "Saturn";
	case PHYSICAL_DISC_DISC_MEGACD: return "MegaCD";
	case PHYSICAL_DISC_DISC_PCECD:  return "TurboGrafx16-CD";
	case PHYSICAL_DISC_DISC_NEOGEO: return "NeoGeoCD";
	case PHYSICAL_DISC_DISC_3DO:    return "3DO";
	case PHYSICAL_DISC_DISC_CDI:    return "CDi";
	case PHYSICAL_DISC_DISC_MDPLUS: return "MDPlus";
	case PHYSICAL_DISC_DISC_SNES:   return "SNES-MSU1";
	default:                        return "";     // audio CDs: left out for now (Phosphor's own player later)
	}
}

static void *read_disc(void *)
{
	disc_info_t r = {};
	r.drive = 1;
	snprintf(r.state, sizeof(r.state), "unreadable");
	if (!physical_disc_open(NULL))
	{
		r.dvd = physical_disc_is_dvd_media();
		toc_t toc;
		memset(&toc, 0, sizeof(toc));
		if (r.dvd) snprintf(r.state, sizeof(r.state), "ready");
		else if (!physical_disc_load_toc(&toc))
		{
			r.tracks = toc.last;
			r.type = physical_disc_identify();
			if (r.type != PHYSICAL_DISC_DISC_AUDIO && r.type != PHYSICAL_DISC_DISC_NONE)
			{
				physical_disc_disc_serial(r.serial, sizeof(r.serial));
				physical_disc_disc_label(r.label, sizeof(r.label));
				snprintf(r.region, sizeof(r.region), "%s", physical_disc_region_name(physical_disc_region()));
			}
			snprintf(r.state, sizeof(r.state), "%s", r.type == PHYSICAL_DISC_DISC_NONE ? "unreadable" : "ready");
		}
		physical_disc_close();
	}
	pthread_mutex_lock(&lk);
	cur = r;
	pthread_mutex_unlock(&lk);
	printf("Phosphor: disc %s %s %s %s\n", r.state, type_key(r.type, r.dvd), r.serial, r.label);
	working = 0;
	return NULL;
}

static void set_state(int drive, const char *state)
{
	pthread_mutex_lock(&lk);
	disc_info_t r = {};
	r.drive = drive;
	snprintf(r.state, sizeof(r.state), "%s", state);
	cur = r;
	pthread_mutex_unlock(&lk);
}

static void json_esc(char *out, size_t n, const char *s)
{
	size_t o = 0;
	for (; *s && o + 2 < n; s++)
	{
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
		else if (c >= 0x20 && c < 0x7f) out[o++] = c;
	}
	out[o] = 0;
}

void phosphor_disc_status(char *out, size_t n, int event)
{
	pthread_mutex_lock(&lk);
	disc_info_t r = cur;
	pthread_mutex_unlock(&lk);
	char serial[64], label[80];
	json_esc(serial, sizeof(serial), r.serial);
	json_esc(label, sizeof(label), r.label);
	const char *mgl = !strcmp(r.state, "ready") ? mgl_for(r.type, r.dvd) : "";
	const char *sys = r.dvd ? "DVD" : (r.type == PHYSICAL_DISC_DISC_NONE ? "" : physical_disc_console_name(r.type));
	snprintf(out, n, "{%s,\"drive\":%s,\"state\":\"%s\",\"type\":\"%s\",\"system\":\"%s\",\"serial\":\"%s\","
		"\"label\":\"%s\",\"region\":\"%s\",\"tracks\":%d,\"playable\":%s,\"mgl\":\"%s\"}",
		event ? "\"event\":\"disc\"" : "\"ok\":true", r.drive ? "true" : "false", r.state, type_key(r.type, r.dvd), sys,
		serial, label, r.region, r.tracks, *mgl ? "true" : "false", mgl);
}

static int eject(void)
{
	for (int i = 0; i < 8; i++)
	{
		char dev[16];
		snprintf(dev, sizeof(dev), "/dev/sr%d", i);
		int fd = open(dev, O_RDONLY | O_NONBLOCK);
		if (fd < 0) continue;
		ioctl(fd, CDROM_LOCKDOOR, 0);
		int rc = ioctl(fd, CDROMEJECT, 0);
		close(fd);
		return rc ? -1 : 0;
	}
	return -1;
}

int phosphor_disc_command(const char *line, char *out, size_t n)
{
	if (!strcmp(line, "disc")) { phosphor_disc_status(out, n, 0); return 1; }
	if (!strcmp(line, "disc eject"))
	{
		if (working) { snprintf(out, n, "{\"ok\":false,\"error\":\"the drive is reading the disc, try again in a moment\"}"); return 1; }
		if (!is_menu()) { snprintf(out, n, "{\"ok\":false,\"error\":\"a game is running\"}"); return 1; }
		physical_disc_tray_release();
		snprintf(out, n, eject() ? "{\"ok\":false,\"error\":\"no drive, or it didn't open\"}" : "{\"ok\":true}");
		return 1;
	}
	return 0;
}

void phosphor_disc_poll(void (*send_event)(const char *))
{
	if (worker_joinable && !working) { pthread_join(worker, NULL); worker_joinable = 0; }
	if (!is_menu() || working || !CheckTimer(next_poll)) goto events;
	next_poll = GetTimer(1000);
	{
		physical_disc_tray_t t = physical_disc_tray_status();
		if (t == PHYSICAL_DISC_TRAY_NODRIVE) { identified = 0; set_state(0, "none"); }
		else if (t == PHYSICAL_DISC_TRAY_EMPTY) { identified = 0; set_state(1, "empty"); }
		else if (t == PHYSICAL_DISC_TRAY_NOTREADY) { if (!identified) set_state(1, "reading"); }
		else if (!identified)
		{
			identified = 1;
			set_state(1, "reading");
			physical_disc_tray_release();       // the worker opens the drive itself
			working = 1;
			if (pthread_create(&worker, NULL, read_disc, NULL)) { working = 0; set_state(1, "unreadable"); }
			else worker_joinable = 1;
		}
	}
events:
	char ev[512];
	phosphor_disc_status(ev, sizeof(ev), 1);
	if (strcmp(ev, last_sent))
	{
		snprintf(last_sent, sizeof(last_sent), "%s", ev);
		send_event(ev);
	}
}
