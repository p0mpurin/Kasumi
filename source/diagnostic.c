#include "diagnostic.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* Lines are formatted into a small in-memory buffer and a low-priority
 * thread writes them to the SD card about once a second. Build 64 wrote
 * through a 256 KiB stdio buffer instead: every few minutes of play the
 * whole buffer went to the SD card at once, on whichever thread logged,
 * stalling the stream loop. No thread that logs ever touches the SD now. */
enum {
    DIAGNOSTIC_MAX_LINES = 6000,
    DIAGNOSTIC_CHUNK = 48 * 1024,
    DIAGNOSTIC_LINE = 768,
    WRITER_STACK = 8 * 1024,
};
static unsigned g_lines;
static unsigned g_dropped;
/* The log hit DIAGNOSTIC_MAX_LINES: the writer moves the file aside and
 * starts a new one. It used to stop writing instead, so a long session lost
 * its end, the part a crash report needs (beta.17 report M7NC4E). */
static bool g_roll_pending;
static uint64_t g_started_ms;
static FILE *g_file;
/* Two buffers: loggers append to the active one while the writer drains
 * the other. */
static char g_chunks[2][DIAGNOSTIC_CHUNK];
static size_t g_chunk_used[2];
static int g_active;
/* 1 is the unlocked state, so logging before diagnostic_init cannot hang. */
static LightLock g_log_lock = 1;
/* Serialises SD writes between the writer thread and checkpoints. */
static LightLock g_file_lock = 1;
static Thread g_writer;
static LightEvent g_writer_wake;
static volatile bool g_writer_quit;

/* Swap buffers under the log lock (fast), then write the full one. */
static void write_pending(void)
{
    LightLock_Lock(&g_file_lock);
    LightLock_Lock(&g_log_lock);
    const int full = g_active;
    const size_t size = g_chunk_used[full];
    g_active ^= 1;
    g_chunk_used[g_active] = 0;
    const bool roll = g_roll_pending;
    g_roll_pending = false;
    LightLock_Unlock(&g_log_lock);
    if (roll && g_file) {
        fclose(g_file);
        remove(DIAGNOSTIC_OLDER_PATH);
        rename(DIAGNOSTIC_PATH, DIAGNOSTIC_OLDER_PATH);
        g_file = fopen(DIAGNOSTIC_PATH, "w");
        if (g_file) {
            fputs(APP_NAME " " APP_VERSION " (build " APP_BUILD ") diagnostic, continued "
                  "(earlier lines: kasumi-diagnostic-older.txt)\n", g_file);
            fflush(g_file);
        }
    }
    if (size && g_file) {
        fwrite(g_chunks[full], 1, size, g_file);
        fflush(g_file);
    }
    LightLock_Unlock(&g_file_lock);
}

static void writer_main(void *arg)
{
    (void)arg;
    while (!g_writer_quit) {
        LightEvent_WaitTimeout(&g_writer_wake, 1000000000LL);
        write_pending();
    }
}

void diagnostic_init(void)
{
    LightLock_Init(&g_log_lock);
    LightLock_Init(&g_file_lock);
    LightEvent_Init(&g_writer_wake, RESET_ONESHOT);
    mkdir("sdmc:/3ds", 0777);
    mkdir(APP_DATA_DIR, 0777);
    g_lines = g_dropped = 0;
    g_chunk_used[0] = g_chunk_used[1] = 0;
    g_active = 0;
    g_started_ms = osGetTime();
    if (g_file) fclose(g_file);
    /* Keep the last run's log instead of overwriting it: after a crash or
     * freeze that is the one worth reading. */
    remove(DIAGNOSTIC_PREVIOUS_PATH);
    rename(DIAGNOSTIC_PATH, DIAGNOSTIC_PREVIOUS_PATH);
    remove(DIAGNOSTIC_OLDER_PATH);
    g_roll_pending = false;
    g_file = fopen(DIAGNOSTIC_PATH, "w");
    if (!g_file) return;
    fputs(APP_NAME " " APP_VERSION " (build " APP_BUILD ") diagnostic\n", g_file);
    fputs("Privacy: tokens, authorization headers, full SDP and ICE passwords are excluded.\n", g_file);
    fflush(g_file);
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    g_writer_quit = false;
    /* Below every other Kasumi thread: it only runs when the app is idle. */
    g_writer = threadCreate(writer_main, NULL, WRITER_STACK,
                            priority + 4 > 0x3F ? 0x3F : priority + 4, -2, false);
}

void diagnostic_close(void)
{
    if (g_writer) {
        g_writer_quit = true;
        LightEvent_Signal(&g_writer_wake);
        threadJoin(g_writer, U64_MAX);
        threadFree(g_writer);
        g_writer = NULL;
    }
    /* Both buffers may hold lines. */
    write_pending();
    write_pending();
    if (!g_file) return;
    fclose(g_file);
    g_file = NULL;
}

/* Before risky steps: get the lines so far to the card soon. The writer
 * thread does it; the caller used to, and on a slow card each checkpoint
 * took seconds. The decoder's start-up has nine of them, and they held the
 * stream (and the menus waiting on it) 2-3 s on such cards (beta.36
 * export). Before the writer starts, write directly as before. */
void diagnostic_checkpoint(void)
{
    if (g_writer) LightEvent_Signal(&g_writer_wake);
    else write_pending();
}

void diagnostic_vlog(const char *component, const char *format, va_list args)
{
    char line[DIAGNOSTIC_LINE];
    int n = snprintf(line, sizeof(line), "ms=%llu [%s] ",
                     (unsigned long long)(osGetTime() - g_started_ms),
                     component ? component : "APP");
    if (n < 0) return;
    if ((size_t)n < sizeof(line)) {
        const int body = vsnprintf(line + n, sizeof(line) - (size_t)n, format, args);
        if (body > 0) n += body;
    }
    if ((size_t)n > sizeof(line) - 2) n = (int)sizeof(line) - 2;
    line[n++] = '\n';

    /* The UI thread, decoder and network worker all log; keep lines whole. */
    LightLock_Lock(&g_log_lock);
    if (g_lines >= DIAGNOSTIC_MAX_LINES) {
        g_lines = 0;
        g_roll_pending = true;
    }
    {
        size_t *used = &g_chunk_used[g_active];
        if (*used + (size_t)n <= DIAGNOSTIC_CHUNK) {
            memcpy(g_chunks[g_active] + *used, line, (size_t)n);
            *used += (size_t)n;
            ++g_lines;
        } else {
            ++g_dropped;
        }
    }
    const bool nearly_full = g_chunk_used[g_active] > DIAGNOSTIC_CHUNK / 2;
    LightLock_Unlock(&g_log_lock);
    if (nearly_full && g_writer) LightEvent_Signal(&g_writer_wake);
}

void diagnostic_log(const char *component, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    diagnostic_vlog(component, format, args);
    va_end(args);
}

/* ---- Flags ---------------------------------------------------------------- */

enum { FLAG_SLOTS = 32, FLAG_CODE = 24 };
static struct {
    char code[FLAG_CODE];
    unsigned count;
    bool taken;
} g_flags[FLAG_SLOTS];
static unsigned g_flag_count;
static LightLock g_flag_lock = 1;

void diagnostic_flag(const char *code, const char *format, ...)
{
    if (!code || !code[0]) code = "unknown";
    unsigned count = 0;
    LightLock_Lock(&g_flag_lock);
    unsigned i = 0;
    while (i < g_flag_count && strncmp(g_flags[i].code, code, FLAG_CODE - 1)) ++i;
    if (i == g_flag_count && g_flag_count < FLAG_SLOTS) {
        snprintf(g_flags[i].code, FLAG_CODE, "%s", code);
        g_flags[i].count = 0;
        g_flags[i].taken = false;
        ++g_flag_count;
    }
    if (i < g_flag_count) count = ++g_flags[i].count;
    LightLock_Unlock(&g_flag_lock);
    /* The first few of each, then every 50th: a flag that fires every frame
     * must not push the rest of the log out. */
    if (count > 5 && count % 50) return;
    char text[DIAGNOSTIC_LINE - 64];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format ? format : "", args);
    va_end(args);
    diagnostic_log("FLAG", "%s #%u: %s", code, count ? count : 1, text);
}

const char *diagnostic_take_new_flag(void)
{
    const char *code = NULL;
    LightLock_Lock(&g_flag_lock);
    for (unsigned i = 0; i < g_flag_count && !code; ++i) {
        if (g_flags[i].taken) continue;
        g_flags[i].taken = true;
        code = g_flags[i].code;
    }
    LightLock_Unlock(&g_flag_lock);
    return code;
}

void diagnostic_flags_summary(char *out, size_t size)
{
    if (!out || !size) return;
    out[0] = '\0';
    size_t used = 0;
    LightLock_Lock(&g_flag_lock);
    for (unsigned i = 0; i < g_flag_count && used + 1 < size; ++i) {
        const int n = snprintf(out + used, size - used, "%s%s x%u", used ? ", " : "", g_flags[i].code,
                               g_flags[i].count);
        if (n < 0) break;
        used += (size_t)n;
    }
    LightLock_Unlock(&g_flag_lock);
    if (used >= size) out[size - 1] = '\0';
}
