#include "file_worker.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "diagnostic.h"

enum { JOB_SAVE, JOB_APPEND, JOB_REMOVE };
enum { QUEUE_MAX = 24, PATH_MAX_LENGTH = 96 };
/* A write slower than this is logged with the file's name. */
#define SLOW_WRITE_MS 400

typedef struct {
    int kind;
    char path[PATH_MAX_LENGTH];
    json_t *data;
    size_t keep, flags;
} Job;

static Job g_queue[QUEUE_MAX];
static unsigned g_head, g_count;
/* Jobs taken off the queue but not finished (for flush). */
static unsigned g_running;
static LightLock g_lock = 1;
static LightEvent g_wake, g_idle;
static Thread g_thread;
static bool g_started, g_quit;
static volatile unsigned g_failed_writes;

unsigned file_worker_failed_writes(void) { return g_failed_writes; }

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void run(Job *job)
{
    const u64 start = osGetTime();
    switch (job->kind) {
    case JOB_SAVE:
        if (json_dump_file(job->data, job->path, job->flags) != 0) {
            diagnostic_log("FILE", "could not write %s", base_name(job->path));
            ++g_failed_writes;
        }
        break;
    case JOB_APPEND: {
        json_error_t error;
        json_t *list = json_load_file(job->path, 0, &error);
        if (!json_is_array(list)) {
            json_decref(list);
            list = json_array();
        }
        json_array_append(list, job->data);
        while (job->keep && json_array_size(list) > job->keep) json_array_remove(list, 0);
        if (json_dump_file(list, job->path, job->flags) != 0) {
            diagnostic_log("FILE", "could not write %s", base_name(job->path));
            ++g_failed_writes;
        }
        json_decref(list);
        break;
    }
    case JOB_REMOVE:
        remove(job->path);
        break;
    }
    const u64 took = osGetTime() - start;
    if (took >= SLOW_WRITE_MS)
        diagnostic_log("FILE", "%s took %llu ms (slow SD card?)", base_name(job->path), (unsigned long long)took);
    json_decref(job->data);
    job->data = NULL;
}

static void worker_main(void *arg)
{
    (void)arg;
    for (;;) {
        LightLock_Lock(&g_lock);
        if (!g_count) {
            const bool quit = g_quit;
            LightEvent_Signal(&g_idle);
            LightLock_Unlock(&g_lock);
            if (quit) return;
            LightEvent_Wait(&g_wake);
            continue;
        }
        Job job = g_queue[g_head];
        g_head = (g_head + 1) % QUEUE_MAX;
        --g_count;
        ++g_running;
        LightLock_Unlock(&g_lock);
        run(&job);
        LightLock_Lock(&g_lock);
        --g_running;
        LightLock_Unlock(&g_lock);
    }
}

static bool start(void)
{
    if (g_started) return g_thread != NULL;
    g_started = true;
    LightEvent_Init(&g_wake, RESET_ONESHOT);
    LightEvent_Init(&g_idle, RESET_STICKY);
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    /* Below the main loop, above the diagnostic writer. */
    g_thread = threadCreate(worker_main, NULL, 32 * 1024, priority + 2 > 0x3F ? 0x3F : priority + 2, -2, false);
    return g_thread != NULL;
}

void file_worker_init(void) { start(); }

static void enqueue(int kind, const char *path, json_t *data, size_t keep, size_t flags)
{
    Job job = { .kind = kind, .data = data, .keep = keep, .flags = flags };
    snprintf(job.path, sizeof(job.path), "%s", path);
    if (g_quit || !start()) {
        /* No thread (or exiting): do it here. */
        run(&job);
        return;
    }
    LightLock_Lock(&g_lock);
    /* A newer save of a file still waiting replaces the old one, if
     * nothing else was queued for that file after it. */
    if (kind == JOB_SAVE) {
        for (unsigned i = g_count; i-- > 0;) {
            Job *queued = &g_queue[(g_head + i) % QUEUE_MAX];
            if (strcmp(queued->path, job.path)) continue;
            if (queued->kind == JOB_SAVE) {
                json_decref(queued->data);
                queued->data = data;
                queued->flags = flags;
                LightLock_Unlock(&g_lock);
                return;
            }
            break;
        }
    }
    if (g_count == QUEUE_MAX) {
        /* Full (the card has stalled for a long time): wait for room
         * rather than lose a write. */
        LightLock_Unlock(&g_lock);
        file_worker_flush();
        LightLock_Lock(&g_lock);
    }
    g_queue[(g_head + g_count) % QUEUE_MAX] = job;
    ++g_count;
    LightEvent_Clear(&g_idle);
    LightLock_Unlock(&g_lock);
    LightEvent_Signal(&g_wake);
}

void file_worker_save_json(const char *path, json_t *root, size_t flags)
{
    if (!root) return;
    enqueue(JOB_SAVE, path, root, 0, flags);
}

void file_worker_append_json(const char *path, json_t *record, size_t keep, size_t flags)
{
    if (!record) return;
    enqueue(JOB_APPEND, path, record, keep, flags);
}

void file_worker_remove(const char *path)
{
    enqueue(JOB_REMOVE, path, NULL, 0, 0);
}

void file_worker_flush(void)
{
    if (!g_thread) return;
    for (;;) {
        LightLock_Lock(&g_lock);
        const bool idle = !g_count && !g_running;
        LightLock_Unlock(&g_lock);
        if (idle) return;
        LightEvent_WaitTimeout(&g_idle, 50000000LL);
    }
}

void file_worker_exit(void)
{
    if (!g_thread) return;
    file_worker_flush();
    g_quit = true;
    LightEvent_Signal(&g_wake);
    threadJoin(g_thread, U64_MAX);
    threadFree(g_thread);
    g_thread = NULL;
}
