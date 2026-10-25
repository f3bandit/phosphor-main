// Phosphor: the dock's CD/DVD drive for Phosphor's API (phosphor_disc.cpp). GPL-3.0.
#ifndef PHOSPHOR_DISC_H
#define PHOSPHOR_DISC_H
#include <stddef.h>

void phosphor_disc_poll(void (*send_event)(const char *));     // from phosphor_poll
int phosphor_disc_command(const char *line, char *out, size_t n);  // 1: handled, reply in out
void phosphor_disc_status(char *out, size_t n, int event);
int phosphor_disc_busy(void);   // reading a disc: upstream's menu tick leaves the drive alone

#endif
