#include "platform.h"

#include <3ds.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

u64 plat_time_ms(void) { return osGetTime(); }
u64 plat_ticks(void) { return svcGetSystemTick(); }
void plat_sleep_ns(s64 ns) { svcSleepThread(ns); }

void plat_lock_init(PlatLock *lock) { LightLock_Init(lock); }
void plat_lock(PlatLock *lock) { LightLock_Lock(lock); }
void plat_unlock(PlatLock *lock) { LightLock_Unlock(lock); }

void plat_rlock_init(PlatRecursiveLock *lock) { RecursiveLock_Init(lock); }
void plat_rlock(PlatRecursiveLock *lock) { RecursiveLock_Lock(lock); }
void plat_runlock(PlatRecursiveLock *lock) { RecursiveLock_Unlock(lock); }

void plat_event_init(PlatEvent *event, bool sticky)
{
    LightEvent_Init(event, sticky ? RESET_STICKY : RESET_ONESHOT);
}
void plat_event_signal(PlatEvent *event) { LightEvent_Signal(event); }
void plat_event_clear(PlatEvent *event) { LightEvent_Clear(event); }
void plat_event_wait(PlatEvent *event) { LightEvent_Wait(event); }
int plat_event_wait_timeout(PlatEvent *event, s64 timeout_ns)
{
    return LightEvent_WaitTimeout(event, timeout_ns);
}

int plat_thread_priority(void)
{
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    return priority;
}

PlatThread plat_thread_create(void (*entry)(void *), void *arg, size_t stack_size,
                              int priority, int core, bool detached)
{
    return threadCreate(entry, arg, stack_size, priority, core, detached);
}

void plat_thread_join(PlatThread thread)
{
    threadJoin(thread, U64_MAX);
    threadFree(thread);
}

int plat_wifi_strength(void) { return osGetWifiStrength(); }

bool plat_wifi_ssid(char *out, size_t size)
{
    char ssid[64];
    memset(ssid, 0, sizeof(ssid));
    if (R_FAILED(ACU_GetSSID(ssid))) ssid[0] = '\0';
    ssid[32] = '\0';
    snprintf(out, size, "%s", ssid);
    return out[0] != '\0';
}

bool plat_ip_info(u32 *ip, u32 *mask, u32 *broadcast)
{
    struct in_addr a, m, b;
    if (R_FAILED(SOCU_GetIPInfo(&a, &m, &b))) return false;
    if (ip) *ip = ntohl(a.s_addr);
    if (mask) *mask = ntohl(m.s_addr);
    if (broadcast) *broadcast = ntohl(b.s_addr);
    return true;
}

bool plat_random_bytes(void *out, size_t size)
{
    if (R_FAILED(psInit())) return false;
    const Result rc = PS_GenerateRandomBytes(out, size);
    psExit();
    return R_SUCCEEDED(rc);
}

bool plat_headset_connected(void) { return osIsHeadsetConnected(); }
