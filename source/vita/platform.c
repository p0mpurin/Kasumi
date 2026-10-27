#include "platform.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/netctl.h>
#include <psp2/rtc.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(((PlatLock *)0)->work) >= sizeof(SceKernelLwMutexWork), "PlatLock must hold an LwMutex");

/* Microseconds from 1 January 0001 (the RTC's epoch) to 1 January 1900
 * (osGetTime's): 693,595 days. */
#define TICKS_TO_1900 (693595ull * 86400ull * 1000000ull)

u64 plat_time_ms(void)
{
    SceRtcTick tick;
    if (sceRtcGetCurrentTick(&tick) < 0) return sceKernelGetProcessTimeWide() / 1000;
    return (tick.tick - TICKS_TO_1900) / 1000;
}

u64 plat_ticks(void) { return sceKernelGetProcessTimeWide(); }

void plat_sleep_ns(s64 ns)
{
    sceKernelDelayThread(ns > 0 ? (SceUInt)(ns / 1000) : 0);
}

static SceKernelLwMutexWork *lw(PlatLock *lock) { return (SceKernelLwMutexWork *)(void *)lock->work; }

/* Made on first use: 0 none, 1 being made, 2 ready. */
static void lock_ready(PlatLock *lock)
{
    if (__atomic_load_n(&lock->state, __ATOMIC_ACQUIRE) == 2) return;
    int expected = 0;
    if (__atomic_compare_exchange_n(&lock->state, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        sceKernelCreateLwMutex(lw(lock), "kasumi", lock->recursive ? SCE_KERNEL_MUTEX_ATTR_RECURSIVE : 0, 0, NULL);
        __atomic_store_n(&lock->state, 2, __ATOMIC_RELEASE);
        return;
    }
    while (__atomic_load_n(&lock->state, __ATOMIC_ACQUIRE) != 2) sceKernelDelayThread(10);
}

/* Init again on a lock already made (gallery.c does, on every open) keeps
 * the kernel object instead of leaking it. */
void plat_lock_init(PlatLock *lock)
{
    if (__atomic_load_n(&lock->state, __ATOMIC_ACQUIRE) == 2) return;
    memset(lock, 0, sizeof(*lock));
    lock_ready(lock);
}

void plat_lock(PlatLock *lock)
{
    lock_ready(lock);
    sceKernelLockLwMutex(lw(lock), 1, NULL);
}

void plat_unlock(PlatLock *lock) { sceKernelUnlockLwMutex(lw(lock), 1); }

void plat_rlock_init(PlatRecursiveLock *lock)
{
    if (__atomic_load_n(&lock->state, __ATOMIC_ACQUIRE) == 2) return;
    memset(lock, 0, sizeof(*lock));
    lock->recursive = 1;
    lock_ready(lock);
}

void plat_rlock(PlatRecursiveLock *lock) { plat_lock(lock); }
void plat_runlock(PlatRecursiveLock *lock) { plat_unlock(lock); }

void plat_event_init(PlatEvent *event, bool sticky)
{
    event->sticky = sticky;
    /* Made already: start it again cleared rather than make another. */
    if (event->id > 0) {
        sceKernelClearEventFlag(event->id, 0);
        return;
    }
    event->id = sceKernelCreateEventFlag("kasumi", SCE_EVENT_WAITMULTIPLE, 0, NULL);
}

void plat_event_signal(PlatEvent *event) { sceKernelSetEventFlag(event->id, 1); }
/* sceKernelClearEventFlag keeps the bits given: 0 clears them all. */
void plat_event_clear(PlatEvent *event) { sceKernelClearEventFlag(event->id, 0); }

/* A one-shot event clears itself as the waiter wakes. */
static unsigned wait_mode(const PlatEvent *event)
{
    return SCE_EVENT_WAITAND | (event->sticky ? 0 : SCE_EVENT_WAITCLEAR);
}

void plat_event_wait(PlatEvent *event)
{
    unsigned bits = 0;
    sceKernelWaitEventFlag(event->id, 1, wait_mode(event), &bits, NULL);
}

int plat_event_wait_timeout(PlatEvent *event, s64 timeout_ns)
{
    unsigned bits = 0;
    SceUInt timeout = timeout_ns > 0 ? (SceUInt)(timeout_ns / 1000) : 0;
    return sceKernelWaitEventFlag(event->id, 1, wait_mode(event), &bits, &timeout) < 0 ? 1 : 0;
}

struct PlatThreadImpl {
    SceUID id;
    void (*entry)(void *);
    void *arg;
    bool detached;
};

/* The main thread runs at 160 of the Vita's 64-191; each 3DS step (0x30 is
 * the main thread there) is four Vita steps, lower still more urgent. */
#define VITA_MAIN_PRIORITY 160

int plat_thread_priority(void) { return 0x30; }

static int thread_main(SceSize args, void *argp)
{
    (void)args;
    struct PlatThreadImpl *thread = *(struct PlatThreadImpl **)argp;
    thread->entry(thread->arg);
    if (thread->detached) {
        free(thread);
        sceKernelExitDeleteThread(0);
    }
    return 0;
}

PlatThread plat_thread_create(void (*entry)(void *), void *arg, size_t stack_size,
                              int priority, int core, bool detached)
{
    struct PlatThreadImpl *thread = calloc(1, sizeof(*thread));
    if (!thread) return NULL;
    thread->entry = entry;
    thread->arg = arg;
    thread->detached = detached;
    int vita_priority = VITA_MAIN_PRIORITY + (priority - 0x30) * 4;
    if (vita_priority < 64) vita_priority = 64;
    if (vita_priority > 191) vita_priority = 191;
    /* Core 2 for the stream (as on the 3DS), anywhere else otherwise. */
    const int affinity = core == PLAT_CORE_MEDIA ? SCE_KERNEL_CPU_MASK_USER_2 : SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT;
    /* The 3DS's stacks were sized for its libc and mbedTLS; OpenSSL's
     * handshakes on the Vita need far more. */
    if (stack_size < 256 * 1024) stack_size = 256 * 1024;
    thread->id = sceKernelCreateThread("kasumi", thread_main, vita_priority, (SceSize)stack_size, 0, affinity, NULL);
    if (thread->id < 0) {
        free(thread);
        return NULL;
    }
    if (sceKernelStartThread(thread->id, sizeof(thread), &thread) < 0) {
        sceKernelDeleteThread(thread->id);
        free(thread);
        return NULL;
    }
    return detached ? (PlatThread)1 : thread;
}

void plat_thread_join(PlatThread thread)
{
    if (!thread || thread == (PlatThread)1) return;
    sceKernelWaitThreadEnd(thread->id, NULL, NULL);
    sceKernelDeleteThread(thread->id);
    free(thread);
}

static bool netctl_info(int code, SceNetCtlInfo *info)
{
    memset(info, 0, sizeof(*info));
    return sceNetCtlInetGetInfo(code, info) >= 0;
}

int plat_wifi_strength(void)
{
    SceNetCtlInfo info;
    if (!netctl_info(SCE_NETCTL_INFO_GET_RSSI_PERCENTAGE, &info)) return 0;
    const unsigned percent = info.rssi_percentage;
    return percent >= 70 ? 3 : percent >= 40 ? 2 : percent > 0 ? 1 : 0;
}

bool plat_wifi_ssid(char *out, size_t size)
{
    SceNetCtlInfo info;
    if (!netctl_info(SCE_NETCTL_INFO_GET_SSID, &info)) info.ssid[0] = '\0';
    info.ssid[sizeof(info.ssid) - 1] = '\0';
    snprintf(out, size, "%s", info.ssid);
    return out[0] != '\0';
}

bool plat_ip_info(u32 *ip, u32 *mask, u32 *broadcast)
{
    SceNetCtlInfo info;
    struct in_addr a, m;
    if (!netctl_info(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) || inet_pton(AF_INET, info.ip_address, &a) != 1)
        return false;
    if (!netctl_info(SCE_NETCTL_INFO_GET_NETMASK, &info) || inet_pton(AF_INET, info.netmask, &m) != 1)
        m.s_addr = htonl(0xFFFFFF00u);
    const u32 address = ntohl(a.s_addr), netmask = ntohl(m.s_addr);
    if (ip) *ip = address;
    if (mask) *mask = netmask;
    if (broadcast) *broadcast = address | ~netmask;
    return true;
}

bool plat_random_bytes(void *out, size_t size)
{
    /* At most 64 bytes a call. */
    for (size_t done = 0; done < size;) {
        const size_t n = size - done < 64 ? size - done : 64;
        if (sceKernelGetRandomNumber((u8 *)out + done, (SceSize)n) < 0) return false;
        done += n;
    }
    return true;
}

/* No public call says whether headphones are in; voice chat's echo
 * canceller simply stays on. */
bool plat_headset_connected(void) { return false; }
