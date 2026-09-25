#pragma once

#include <3ds/types.h>

#define PROBE_PORT 50010

bool probe_init(void);
bool probe_start(void);
void probe_stop(void);
bool probe_is_running(void);
void probe_poll(void);
void probe_print_status(void);
void probe_exit(void);
