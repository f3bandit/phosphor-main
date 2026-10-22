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
int fpga_load_rbf(const char *n, const char *, const char *) { snprintf(g_loaded, sizeof(g_loaded), "rbf:%s", n); return 0; }
int xml_load(const char *x) { snprintf(g_loaded, sizeof(g_loaded), "xml:%s", x); return 0; }
int isXmlName(const char *p) { const char *e = strrchr(p, '.'); return e && (!strcasecmp(e, ".mgl") || !strcasecmp(e, ".mra")) ? 2 : 0; }
void menu_phosphor_screen(int on) { g_screen_req = on; g_fb = on; }
unsigned long GetTimer(unsigned long off) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000 + off; }
