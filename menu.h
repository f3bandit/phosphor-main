#ifndef MENU_H
#define MENU_H

#include <inttypes.h>

void SelectFile(const char* path, const char* pFileExt, int Options, unsigned char MenuSelect, unsigned char MenuCancel, int recent_idx = -1);

void HandleUI(void);
void menu_key_set(unsigned int c);
void menu_process_save();
void PrintDirectory(int expand = 0);
void ScrollLongName(void);

void ProgressMessage(const char* title = 0, const char* text = 0, int current = 0, int max = 0);
void InfoMessage(const char *message, int timeout = 2000, const char *title = "Message");
void Info(const char *message, int timeout = 2000, int width = 0, int height = 0, int frame = 0);
void InfoAt(const char *message, int timeout = 2000, int y_pos = 10, int frame = 0);

// Horizontal placement for InfoAligned()
#define INFO_ALIGN_LEFT   0
#define INFO_ALIGN_CENTER 1
#define INFO_ALIGN_RIGHT  2

// Same popup as Info(), but the window is placed left / centered / right on the
// top row instead of always hugging the left edge. Used by the RetroAchievements
// popups (retroachievements.cfg: popup_position) so they stay visible when the
// display crops the sides (e.g. HDMI forced to 4:3 on a TV).
void InfoAligned(const char *message, int timeout = 2000, int align = INFO_ALIGN_LEFT, int frame = 0);
void MenuHide();
int phosphor_osd_menu(const char *title, const char *text, int sel);   // -1: MiSTer's menu has the OSD
int phosphor_osd_own(void);
void phosphor_osd_menu_off(void);
int phosphor_osd_menu_shown(void);
void SelectINI();

// Launch a script command through MiSTer's normal script lifecycle.
// Returns non-zero when the command was queued.
int menu_launch_script_command(const char *command, const char *title, const char *process_name);
int menu_launch_graphical_script_command(const char *command, const char *title, const char *process_name);

void open_joystick_setup();
int menu_lightgun_cb(int idx, uint16_t type, uint16_t code, int value);

int menu_allow_cfg_switch();
void StoreIdx_F(int idx, const char *path);
void StoreIdx_S(int idx, const char *path);
const char *GetIdx_S(int idx);

int menu_present();

#endif
