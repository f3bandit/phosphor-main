// stand-ins for Main's functions, to test phosphor.cpp on its own
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
int g_menu = 1, g_osd = 0, g_fb = 0, g_screen_req = -9;
char g_core[64] = "MENU";
char g_loaded[512];
char is_menu() { return g_menu; }
char user_io_osd_is_visible() { return g_osd; }
char *user_io_get_core_name(int) { return g_core; }
int video_fb_state() { return g_fb; }
int g_vt = 1, g_fbcon;
void video_fb_console() { g_fbcon++; }
void video_fb_console_done() {}
int video_chvt(int n) { if (n) g_vt = n; return g_vt; }
int fpga_load_rbf(const char *n, const char *, const char *) { snprintf(g_loaded, sizeof(g_loaded), "rbf:%s", n); return 0; }
int xml_load(const char *x) { snprintf(g_loaded, sizeof(g_loaded), "xml:%s", x); return 0; }
int isXmlName(const char *p) { const char *e = strrchr(p, '.'); return e && (!strcasecmp(e, ".mgl") || !strcasecmp(e, ".mra")) ? 2 : 0; }
void menu_phosphor_screen(int on) { g_screen_req = on; g_fb = on; }
unsigned long GetTimer(unsigned long off) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000 + off; }

// the Phosphor menu core's extension (phosphor-menu), as Main sees it over SPI. Like the real bus, each
// word's transfer returns the core's answer to that same word: word 0 (the command) returns the first
// reply word, word k (the k-th after the command) the next.
int g_ext = 1;                    // 0: the stock menu core
uint16_t g_ext_mode;
uint8_t g_card[1024 * 128];
int g_card_present = 1, g_card_busy_polls;
uint16_t g_pad1_btn = 0xFFEF;     // Up held (active low)
int g_pad_cmds[8], g_pad_ncmd;
static int x_cmd = -1, x_k, x_wr, x_port, x_sec, x_res, x_done, x_busy;
static uint16_t x_buf[64];
void EnableIO() {}
void DisableIO() { x_cmd = -1; }
uint16_t fpga_spi(uint16_t v);
uint16_t spi_uio_cmd_cont(uint16_t c) { x_cmd = c; x_k = -1; return fpga_spi(c); }
uint16_t fpga_spi(uint16_t v)
{
	int k = ++x_k;      // 0: the command word
	if (!g_ext || !g_menu) return 0;
	switch (x_cmd)
	{
	case 0x80: return k == 0 ? 0x5048 : k == 1 ? 1 : g_ext_mode;
	case 0x81: if (k == 0) return 0x5048; if (k == 1) { uint16_t o = g_ext_mode; g_ext_mode = v; return o; } return 0;
	case 0x82: {
		uint16_t w[9] = { g_ext_mode, 0x0173, g_pad1_btn, 0x8080, 0x7F81, 0x0000, 0xFFFF, 0, 0 };
		if (!(g_ext_mode & 1)) w[1] = 0;
		return k <= 8 ? w[k] : 0;
	}
	case 0x83:
		if (x_busy || k == 0) return 0;
		if (k == 1) { x_port = v >> 8; x_wr = v & 1; }
		if (k == 2) { x_sec = v; if (!x_wr) { x_busy = 1; x_done = 0; } }
		if (x_wr && k >= 3 && k <= 66) x_buf[k - 3] = v;
		if (x_wr && k == 66) { x_busy = 1; x_done = 0; }
		if (x_busy)
		{
			// the card answers (the busy flag stays up for a few status reads)
			x_res = (!g_card_present || x_port) ? 1 : 0;
			if (!x_res) for (int i = 0; i < 64; i++)
			{
				uint8_t *b = g_card + x_sec * 128 + i * 2;
				if (x_wr) { b[0] = x_buf[i] & 0xFF; b[1] = x_buf[i] >> 8; }
				else x_buf[i] = b[0] | b[1] << 8;
			}
		}
		return 0;
	case 0x84:
		if (k == 0)
		{
			if (x_busy && g_card_busy_polls-- > 0) return 0x8000;
			if (x_busy) { x_busy = 0; x_done = 1; }
			return (x_done ? 0x4000 : 0) | x_res;
		}
		return k <= 64 ? x_buf[k - 1] : 0;
	case 0x85: if (k == 1 && g_pad_ncmd < 8) g_pad_cmds[g_pad_ncmd++] = v; return 0;
	}
	return 0;
}
unsigned long CheckTimer(unsigned long t) { return GetTimer(0) > t; }
