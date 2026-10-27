#include "platform.h"
#include "diagnostic.h"

#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* newlib's heap (the default is 32 MiB) and the main thread's stack:
 * libcurl with OpenSSL, the library and cover art want more room than a
 * 3DS gives. Video frames live in the GPU's own memory, outside the heap. */
int _newlib_heap_size_user = 128 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 1024 * 1024;

/* The network stack's own memory, as most Vita streaming clients give it. */
#define NET_MEMORY (1024 * 1024)

static void *g_net_memory;
static bool g_net_ready, g_netctl_ready;

bool plat_services_init(char *error, size_t error_size)
{
    SceAppUtilInitParam init;
    SceAppUtilBootParam boot;
    memset(&init, 0, sizeof(init));
    memset(&boot, 0, sizeof(boot));
    sceAppUtilInit(&init, &boot);
    int rc = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    if (rc < 0) {
        snprintf(error, error_size, "Network module load 0x%08X", (unsigned)rc);
        return false;
    }
    g_net_memory = malloc(NET_MEMORY);
    if (!g_net_memory) {
        snprintf(error, error_size, "Network memory allocation failed");
        return false;
    }
    SceNetInitParam param = { g_net_memory, NET_MEMORY, 0 };
    rc = sceNetInit(&param);
    /* Already up (another app's library left it running) is fine. */
    if (rc < 0 && (unsigned)rc != 0x80410110u) {
        snprintf(error, error_size, "Network init 0x%08X", (unsigned)rc);
        return false;
    }
    g_net_ready = true;
    rc = sceNetCtlInit();
    if (rc < 0 && (unsigned)rc != 0x80412102u) {
        snprintf(error, error_size, "Network control init 0x%08X", (unsigned)rc);
        return false;
    }
    g_netctl_ready = true;
    return true;
}

void plat_services_exit(void)
{
    if (g_netctl_ready) sceNetCtlTerm();
    g_netctl_ready = false;
    if (g_net_ready) sceNetTerm();
    g_net_ready = false;
    free(g_net_memory);
    g_net_memory = NULL;
    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
    sceAppUtilShutdown();
}

/* Every Vita (and PS TV) has the hardware decoder. */
bool plat_model_supported(void) { return true; }

void plat_full_speed(void)
{
    /* The highest clocks homebrew may ask for without plugins. */
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
}

static bool g_sleep_allowed = true;
static u64 g_last_tick_us;
static void (*g_system_handler)(PlatSystemEvent event);

bool plat_main_loop(void)
{
    const u64 now = sceKernelGetProcessTimeWide();
    /* The Vita sleeps after a while without input; a queue or a game
     * keeps it awake by telling it now and then that the app is busy. */
    if (!g_sleep_allowed && now - g_last_tick_us >= 1000000) {
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        g_last_tick_us = now;
    }
    /* The app is simply frozen while the console sleeps or the PS button's
     * menus are open; the system says so only afterwards. */
    SceAppMgrSystemEvent event;
    memset(&event, 0, sizeof(event));
    if (sceAppMgrReceiveSystemEvent(&event) >= 0 && event.systemEvent == SCE_APPMGR_SYSTEMEVENT_ON_RESUME &&
        g_system_handler) {
        g_system_handler(PLAT_SYS_SLEEP);
        g_system_handler(PLAT_SYS_WAKE);
    }
    /* The system closes the app itself (the user ends it from LiveArea). */
    return true;
}

void plat_allow_sleep(bool allowed)
{
    g_sleep_allowed = allowed;
    g_last_tick_us = 0;
}

void plat_on_system_event(void (*handler)(PlatSystemEvent event)) { g_system_handler = handler; }

void plat_battery(u8 *level, bool *charging)
{
    const int percent = scePowerGetBatteryLifePercent();
    /* The 3DS's five steps: 0 empty ... 5 full. */
    *level = percent < 0 ? 5 : (u8)(percent >= 90 ? 5 : percent / 18);
    *charging = scePowerIsBatteryCharging() == SCE_TRUE;
}

bool plat_wifi_connected(void)
{
    int state = 0;
    return sceNetCtlInetGetState(&state) >= 0 && state == SCE_NETCTL_STATE_CONNECTED;
}

/* The Vita runs no background Wi-Fi scans of the kind the 3DS does. */
void plat_wifi_exclusive(bool on) { (void)on; }

u32 plat_device_memory_free(void)
{
    SceKernelFreeMemorySizeInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetFreeMemorySize(&info) < 0) return 64u * 1024 * 1024;
    return (u32)info.size_cdram;
}

/* No notification light, lid or separate backlights on the Vita. */
void plat_notify_light(u32 color) { (void)color; }
bool plat_lid_closed(void) { return false; }
void plat_screens_on(bool on) { (void)on; }
