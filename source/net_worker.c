#include "net_worker.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "diagnostic.h"
#include "http_client.h"
#include "game_art.h"
#include "updater.h"

#define WORKER_STACK_SIZE (128 * 1024)
#define WORKER_IDLE_NS 50000000LL

/* The worker mutates g_work; the UI copies g_shared. Both are ~50 KiB, so
 * they live in static storage rather than on either thread's stack. */
static GfnClient g_work;
static GfnClient g_shared;
static NvstSignal *g_signal;
static unsigned g_version;
static LightLock g_lock;
static Thread g_thread;
static volatile bool g_quit;

static NetJobKind g_pending;
static char g_pending_text[80];
static GfnGame g_pending_game;
static volatile NetJobKind g_running;
static volatile bool g_signal_starting;
static volatile bool g_cancelled;
static NetJobResult g_result;

static void publish(void)
{
    LightLock_Lock(&g_lock);
    memcpy(&g_shared, &g_work, sizeof(g_shared));
    ++g_version;
    LightLock_Unlock(&g_lock);
}

static bool run_job(NetJobKind kind, const char *text, const GfnGame *game)
{
    switch (kind) {
    case NET_JOB_BEGIN_LOGIN: return gfn_begin_login(&g_work);
    case NET_JOB_CANCEL_LOGIN:
        g_work.auth_state = GFN_AUTH_LOGGED_OUT;
        snprintf(g_work.status, sizeof(g_work.status), "Sign-in cancelled");
        return true;
    case NET_JOB_LOAD_LIBRARY:
        if (!gfn_fetch_library(&g_work)) return false;
        game_art_prefetch(g_work.games, (unsigned)g_work.game_count);
        return true;
    case NET_JOB_CONNECTION_TEST: return gfn_connection_test(&g_work);
    case NET_JOB_UPDATE_CHECK: return updater_check(!strcmp(text, "beta"));
    case NET_JOB_UPDATE_INSTALL: return updater_install();
    case NET_JOB_LIBRARY_CACHED:
        if (gfn_library_load(&g_work)) return true;
        return gfn_fetch_library(&g_work);
    case NET_JOB_SEARCH: return gfn_search_catalog(&g_work, text);
    case NET_JOB_START_SESSION: return gfn_start_session(&g_work, game);
    case NET_JOB_STOP_SESSION:
        if (gfn_stop_session(&g_work)) return true;
        /* Keep a stuck session visible so the user can try again. */
        g_work.session_state = GFN_SESSION_ERROR;
        return false;
    case NET_JOB_RESTART_SESSION:
        if (!gfn_stop_session(&g_work)) {
            g_work.session_state = GFN_SESSION_ERROR;
            return false;
        }
        publish();
        return gfn_start_session(&g_work, game);
    case NET_JOB_START_SIGNAL: {
        g_signal_starting = true;
        const bool ok = nvst_signal_start(g_signal, g_work.signaling_url, g_work.session_id);
        snprintf(g_work.status, sizeof(g_work.status), "%s", g_signal->status);
        g_signal_starting = false;
        return ok;
    }
    case NET_JOB_SIGN_OUT:
        gfn_sign_out(&g_work);
        return true;
    case NET_JOB_NONE: break;
    }
    return false;
}

static void worker_main(void *arg)
{
    (void)arg;
    while (!g_quit) {
        LightLock_Lock(&g_lock);
        const NetJobKind kind = g_pending;
        char text[sizeof(g_pending_text)];
        GfnGame game;
        memcpy(text, g_pending_text, sizeof(text));
        memcpy(&game, &g_pending_game, sizeof(game));
        g_pending = NET_JOB_NONE;
        if (kind != NET_JOB_NONE) {
            g_running = kind;
            g_cancelled = false;
        }
        LightLock_Unlock(&g_lock);

        if (kind != NET_JOB_NONE) {
            const u64 started = osGetTime();
            const bool ok = run_job(kind, text, &game);
            diagnostic_log("WORKER", "job=%d ok=%d cancelled=%d ms=%llu", (int)kind, ok ? 1 : 0,
                           g_cancelled ? 1 : 0, (unsigned long long)(osGetTime() - started));
            LightLock_Lock(&g_lock);
            g_result.kind = kind;
            g_result.ok = ok;
            g_result.cancelled = g_cancelled;
            ++g_result.serial;
            g_running = NET_JOB_NONE;
            LightLock_Unlock(&g_lock);
            publish();
            continue;
        }
        /* Periodic work: sign-in polling and queue/setup polling. Both are
         * rate-limited inside the client. */
        gfn_tick(&g_work);
        gfn_session_tick(&g_work);
        /* The client snapshot is ~150 KiB; copying it 20 times a second
         * (and again on the UI thread) was wasted work during play. */
        static u64 last_publish;
        if (osGetTime() - last_publish >= 200) {
            publish();
            last_publish = osGetTime();
        }
        /* Box art only downloads while no game is queued or running, so it
         * never competes with the stream for Wi-Fi. */
        if (!gfn_session_active(&g_work) && game_art_work()) continue;
        svcSleepThread(WORKER_IDLE_NS);
    }
}

bool net_worker_start(const GfnClient *client, NvstSignal *signal)
{
    LightLock_Init(&g_lock);
    memcpy(&g_work, client, sizeof(g_work));
    memcpy(&g_shared, client, sizeof(g_shared));
    g_signal = signal;
    g_quit = false;
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    /* One step below the UI thread on the same core: it runs whenever the UI
     * waits for vblank or naps between network polls, and never preempts it. */
    g_thread = threadCreate(worker_main, NULL, WORKER_STACK_SIZE, priority + 1, -2, false);
    return g_thread != NULL;
}

void net_worker_stop(void)
{
    if (!g_thread) return;
    g_quit = true;
    http_cancel();
    threadJoin(g_thread, U64_MAX);
    threadFree(g_thread);
    g_thread = NULL;
}

bool net_worker_submit(NetJobKind kind, const char *text, const GfnGame *game)
{
    LightLock_Lock(&g_lock);
    const bool accepted = g_pending == NET_JOB_NONE && g_running == NET_JOB_NONE;
    if (accepted) {
        g_pending = kind;
        snprintf(g_pending_text, sizeof(g_pending_text), "%s", text ? text : "");
        if (game) memcpy(&g_pending_game, game, sizeof(g_pending_game));
        /* Report busy immediately, before the worker picks the job up. */
        g_running = kind;
    }
    LightLock_Unlock(&g_lock);
    return accepted;
}

bool net_worker_busy(void) { return g_running != NET_JOB_NONE; }
NetJobKind net_worker_current_job(void) { return g_running; }
bool net_worker_signal_starting(void) { return g_signal_starting; }

void net_worker_cancel(void)
{
    g_cancelled = true;
    http_cancel();
}

bool net_worker_sync(GfnClient *out)
{
    static unsigned seen = ~0u;
    LightLock_Lock(&g_lock);
    const bool changed = g_version != seen;
    if (changed) {
        memcpy(out, &g_shared, sizeof(*out));
        seen = g_version;
    }
    LightLock_Unlock(&g_lock);
    return changed;
}

NetJobResult net_worker_last_result(void)
{
    LightLock_Lock(&g_lock);
    const NetJobResult result = g_result;
    LightLock_Unlock(&g_lock);
    return result;
}

void net_worker_wait_idle(unsigned timeout_ms)
{
    const u64 deadline = osGetTime() + timeout_ms;
    while (net_worker_busy() && osGetTime() < deadline) svcSleepThread(10000000LL);
}
