#pragma once

#include <stdbool.h>

#include "xcloud_net.h"

#define XBOX_CONSOLE_MAX 8

typedef struct {
    char id[48];
    char name[64];
    char type[24];
    XboxPower power;
    bool streaming;
    bool remote_mgmt;
    bool wireless_warn;
    bool out_of_home_warn;
} XboxConsole;

void xbox_consoles_clear(void);
void xbox_consoles_set(const XboxConsole *list, unsigned count);
void xbox_consoles_seed_cached(const XboxConsole *list, unsigned count);
bool xbox_consoles_live(void);
unsigned xbox_consoles_list(XboxConsole *out, unsigned max);
bool xbox_consoles_find(const char *id, XboxConsole *out);
