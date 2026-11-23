#include "xbox_console.h"
#include "platform.h"

#include <string.h>

static PlatLock g_lock = PLAT_LOCK_INIT;
static XboxConsole g_consoles[XBOX_CONSOLE_MAX];
static unsigned g_count;
static bool g_live;

void xbox_consoles_clear(void)
{
    plat_lock(&g_lock);
    memset(g_consoles, 0, sizeof(g_consoles));
    g_count = 0;
    g_live = false;
    plat_unlock(&g_lock);
}

void xbox_consoles_set(const XboxConsole *list, unsigned count)
{
    plat_lock(&g_lock);
    if (count > XBOX_CONSOLE_MAX) count = XBOX_CONSOLE_MAX;
    if (count && list) memcpy(g_consoles, list, count * sizeof(XboxConsole));
    g_count = count;
    g_live = true;
    plat_unlock(&g_lock);
}

void xbox_consoles_seed_cached(const XboxConsole *list, unsigned count)
{
    plat_lock(&g_lock);
    if (!g_live) {
        if (count > XBOX_CONSOLE_MAX) count = XBOX_CONSOLE_MAX;
        if (count && list) memcpy(g_consoles, list, count * sizeof(XboxConsole));
        g_count = count;
    }
    plat_unlock(&g_lock);
}

bool xbox_consoles_live(void)
{
    plat_lock(&g_lock);
    const bool live = g_live;
    plat_unlock(&g_lock);
    return live;
}

unsigned xbox_consoles_list(XboxConsole *out, unsigned max)
{
    plat_lock(&g_lock);
    const unsigned total = g_count;
    if (out && max) {
        const unsigned copy = total < max ? total : max;
        memcpy(out, g_consoles, copy * sizeof(XboxConsole));
    }
    plat_unlock(&g_lock);
    return total;
}

bool xbox_consoles_find(const char *id, XboxConsole *out)
{
    if (!id || !id[0]) return false;
    plat_lock(&g_lock);
    bool found = false;
    for (unsigned i = 0; i < g_count; ++i) {
        if (!strcmp(g_consoles[i].id, id)) {
            if (out) *out = g_consoles[i];
            found = true;
            break;
        }
    }
    plat_unlock(&g_lock);
    return found;
}
