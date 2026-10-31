// Phosphor: hybrid cores (mister-hybrid-platform ports: Maldita Castilla...) run by Phosphor's Main.
// Copyright (C) 2026 f3bandit. GPL-3.0 (see LICENSE). See hybrid.cpp.
#ifndef HYBRID_H
#define HYBRID_H

// from the UI loop, once the FPGA is ready: the loaded core's launcher started (first call), then watched
void hybrid_poll(void);
// before Main restarts or loads another core: the launcher stopped
void hybrid_stop(void);
// main= names the platform's own Main (MiSTer_hybrid): Phosphor's Main stays and does its job
int hybrid_main(const char *main_name);

#endif
