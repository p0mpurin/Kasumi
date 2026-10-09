#include "launch_stats.h"

#include <3ds.h>
#include <jansson.h>
#include <stdio.h>
#include <string.h>

#include "app_paths.h"
#include "diagnostic.h"
#include "file_worker.h"
#include "provider.h"
#include "regions.h"
#include "report.h"

/* Oldest records drop first when the service can't be reached for a while. */
#define LAUNCH_KEEP 30

static struct {
    bool active;
    bool resumed, weak, auto_weak;
    u64 started_at, queued_at, ready_at;
    int first_place;
    bool ads, conflict;
} g_launch;

void launch_begin(bool resumed, bool weak, bool auto_weak)
{
    memset(&g_launch, 0, sizeof(g_launch));
    g_launch.active = true;
    g_launch.resumed = resumed;
    g_launch.weak = weak;
    g_launch.auto_weak = auto_weak;
    g_launch.started_at = osGetTime();
}

bool launch_active(void) { return g_launch.active; }

void launch_track(const GfnClient *client)
{
    if (!g_launch.active) return;
    const u64 now = osGetTime();
    if (client->session_state == GFN_SESSION_QUEUED && client->queue_position > 0) {
        if (!g_launch.queued_at) g_launch.queued_at = now;
        if (!g_launch.first_place) g_launch.first_place = client->queue_position;
    }
    if (client->session_state == GFN_SESSION_READY && !g_launch.ready_at) g_launch.ready_at = now;
    if (client->ads_required) g_launch.ads = true;
    if (client->conflict_found || client->limit_wait) g_launch.conflict = true;
}

void launch_end(const char *outcome, const char *install_id)
{
    if (!g_launch.active) return;
    g_launch.active = false;
    const u64 now = osGetTime();
    const unsigned queue_s = g_launch.queued_at
        ? (unsigned)(((g_launch.ready_at ? g_launch.ready_at : now) - g_launch.queued_at) / 1000) : 0;
    const unsigned ready_s = g_launch.ready_at ? (unsigned)((g_launch.ready_at - g_launch.started_at) / 1000) : 0;
    const unsigned first_frame_ms = g_launch.ready_at && !strcmp(outcome, "ok")
        ? (unsigned)(now - g_launch.ready_at) : 0;
    diagnostic_log("LAUNCH", "outcome=%s queue=%us place=%d ready=%us firstFrame=%ums ads=%d conflict=%d resumed=%d weak=%d",
                   outcome, queue_s, g_launch.first_place, ready_s, first_frame_ms, g_launch.ads,
                   g_launch.conflict, g_launch.resumed, g_launch.weak);
    if (!install_id) return;
    char region[40];
    regions_last_used(region, sizeof(region));
    GfnProvider provider;
    provider_active(&provider);
    json_t *record = json_pack("{s:s,s:s,s:s,s:s,s:s,s:s,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i}",
                               "v", APP_VERSION, "b", APP_BUILD, "i", install_id, "r", region, "o", outcome,
                               "pv", provider.code,
                               "q", (int)queue_s, "qp", g_launch.first_place, "s", (int)ready_s,
                               "ff", (int)first_frame_ms, "ad", g_launch.ads ? 1 : 0,
                               "c", g_launch.conflict ? 1 : 0, "rs", g_launch.resumed ? 1 : 0,
                               "m", g_launch.weak ? 1 : 0, "aw", g_launch.auto_weak ? 1 : 0);
    if (!record) return;
    /* Written in the background: this runs as the picture appears. */
    file_worker_append_json(LAUNCH_PENDING_PATH, record, LAUNCH_KEEP, JSON_COMPACT);
    report_stats_mark_pending(true);
}
