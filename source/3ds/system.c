#include "platform.h"
#include "diagnostic.h"
#include "i18n.h"

#include <3ds.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOC_BUFFER_SIZE (0x100000)
#define SOC_BUFFER_ALIGNMENT (0x1000)

static u32 *g_soc_buffer;
static bool g_soc_ready;
static bool g_ac_ready;
static bool g_ptm_ready;
/* ndm:u: background Wi-Fi work (StreetPass, SpotPass, notifications). */
static bool g_ndm_ready, g_ndm_exclusive;

bool plat_services_init(char *error, size_t error_size)
{
    Result result = acInit();
    if (R_SUCCEEDED(result)) g_ac_ready = true;
    else {
        snprintf(error, error_size, i18n_text("ac:u init 0x%08lX"), (unsigned long)result);
        return false;
    }
    g_ptm_ready = R_SUCCEEDED(ptmuInit());
    g_ndm_ready = R_SUCCEEDED(ndmuInit());
    g_soc_buffer = memalign(SOC_BUFFER_ALIGNMENT, SOC_BUFFER_SIZE);
    if (!g_soc_buffer) {
        snprintf(error, error_size, "1 MiB SOC buffer allocation failed");
        return false;
    }
    result = socInit(g_soc_buffer, SOC_BUFFER_SIZE);
    if (R_FAILED(result)) {
        snprintf(error, error_size, i18n_text("soc:u init 0x%08lX"), (unsigned long)result);
        return false;
    }
    g_soc_ready = true;
    return true;
}

void plat_services_exit(void)
{
    if (g_soc_ready) socExit();
    g_soc_ready = false;
    free(g_soc_buffer);
    g_soc_buffer = NULL;
    if (g_ptm_ready) ptmuExit();
    g_ptm_ready = false;
    if (g_ndm_exclusive) {
        NDMU_UnlockState();
        NDMU_LeaveExclusiveState();
        g_ndm_exclusive = false;
    }
    if (g_ndm_ready) ndmuExit();
    g_ndm_ready = false;
    if (g_ac_ready) acExit();
    g_ac_ready = false;
}

bool plat_model_supported(void)
{
    bool is_new_3ds = false;
    return R_SUCCEEDED(APT_CheckNew3DS(&is_new_3ds)) && is_new_3ds;
}

void plat_full_speed(void) { osSetSpeedupEnable(true); }
bool plat_main_loop(void) { return aptMainLoop(); }
void plat_allow_sleep(bool allowed) { aptSetSleepAllowed(allowed); }

static aptHookCookie g_apt_cookie;
static void (*g_system_handler)(PlatSystemEvent event);

static void apt_hook(APT_HookType hook, void *param)
{
    (void)param;
    PlatSystemEvent event = PLAT_SYS_OTHER;
    if (hook == APTHOOK_ONSLEEP) event = PLAT_SYS_SLEEP;
    else if (hook == APTHOOK_ONSUSPEND) event = PLAT_SYS_SUSPEND;
    else if (hook == APTHOOK_ONWAKEUP) event = PLAT_SYS_WAKE;
    else if (hook == APTHOOK_ONRESTORE) event = PLAT_SYS_RESTORE;
    if (g_system_handler) g_system_handler(event);
}

void plat_on_system_event(void (*handler)(PlatSystemEvent event))
{
    if (handler && !g_system_handler) aptHook(&g_apt_cookie, apt_hook, NULL);
    else if (!handler && g_system_handler) aptUnhook(&g_apt_cookie);
    g_system_handler = handler;
}

void plat_battery(u8 *level, bool *charging)
{
    u8 value = 5, charge = 0;
    if (g_ptm_ready) {
        PTMU_GetBatteryLevel(&value);
        PTMU_GetBatteryChargeState(&charge);
    }
    *level = value;
    *charging = charge != 0;
}

bool plat_wifi_connected(void)
{
    u32 status = 0;
    return R_SUCCEEDED(ACU_GetWifiStatus(&status)) && status != 0;
}

/* The 3DS keeps doing background Wi-Fi work (StreetPass, SpotPass and
 * notification checks) while an app streams, and each scan takes the radio
 * off the access point for a moment. Beta.19 logs: the stream stopped for
 * 200-280 ms at a time, frames were lost, and ~35 packets a minute had to be
 * re-sent on a fast, nearby connection. Moonlight-N3DS takes the Wi-Fi
 * exclusively (infrastructure only, background daemons stopped) for the
 * same reason. Kasumi does it only while a game session runs. */
void plat_wifi_exclusive(bool on)
{
    if (!g_ndm_ready || on == g_ndm_exclusive) return;
    Result result;
    if (on) {
        result = NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE);
        if (R_SUCCEEDED(result)) result = NDMU_LockState();
        g_ndm_exclusive = R_SUCCEEDED(result);
        if (!g_ndm_exclusive) NDMU_LeaveExclusiveState();
    } else {
        NDMU_UnlockState();
        result = NDMU_LeaveExclusiveState();
        g_ndm_exclusive = false;
    }
    diagnostic_log("NET", "exclusive Wi-Fi %s rc=%08lX", on ? "on" : "off", (unsigned long)result);
}

u32 plat_device_memory_free(void) { return linearSpaceFree(); }

bool plat_keyboard(const PlatKeyboard *keyboard, char *text, size_t size)
{
    static SwkbdState swkbd;
    swkbdInit(&swkbd, keyboard->numbers ? SWKBD_TYPE_NUMPAD : SWKBD_TYPE_QWERTY, 2,
              (int)keyboard->max_length);
    if (keyboard->numbers) swkbdSetNumpadKeys(&swkbd, '.', 0);
    swkbdSetHintText(&swkbd, keyboard->hint);
    swkbdSetButton(&swkbd, SWKBD_BUTTON_LEFT, keyboard->cancel, false);
    swkbdSetButton(&swkbd, SWKBD_BUTTON_RIGHT, keyboard->ok, true);
    swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    swkbdSetFeatures(&swkbd, SWKBD_DARKEN_TOP_SCREEN | (keyboard->predictive ? SWKBD_PREDICTIVE_INPUT : 0));
    if (text[0]) swkbdSetInitialText(&swkbd, text);
    return swkbdInputText(&swkbd, text, size) == SWKBD_BUTTON_RIGHT;
}

/* A slow breathing pulse, repeating until turned off. */
void plat_notify_light(u32 color)
{
    if (R_FAILED(mcuHwcInit())) return;
    InfoLedPattern pattern;
    memset(&pattern, 0, sizeof(pattern));
    if (color) {
        const u8 r = color & 0xFF, g = (color >> 8) & 0xFF, b = (color >> 16) & 0xFF;
        pattern.delay = 2;
        pattern.smoothing = 0x40;
        pattern.loopDelay = 0;
        for (int i = 0; i < 32; ++i) {
            const float level = 0.5f - 0.5f * cosf((float)i / 32.0f * 2.0f * (float)M_PI);
            pattern.redPattern[i] = (u8)(r * level);
            pattern.greenPattern[i] = (u8)(g * level);
            pattern.bluePattern[i] = (u8)(b * level);
        }
    }
    MCUHWC_SetInfoLedPattern(&pattern);
    mcuHwcExit();
}

bool plat_lid_closed(void)
{
    u8 open = 1;
    if (R_FAILED(PTMU_GetShellState(&open))) return false;
    return open == 0;
}

void plat_screens_on(bool on)
{
    if (R_FAILED(gspLcdInit())) return;
    if (on) GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTH);
    else GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTH);
    gspLcdExit();
}
