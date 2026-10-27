#include "regions.h"
#include "platform.h"

#include <jansson.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_paths.h"
#include "diagnostic.h"
#include "http_client.h"
#include "provider.h"

#define REGIONS_PATH APP_DATA_DIR "/regions.json"
/* Servers don't move; a network's measurement stays good for a week. */
#define REGIONS_MAX_AGE (7 * 24 * 3600)
/* Version 2 measures with HTTP round trips. Version 1 timed a non-blocking
 * TCP connect, which the 3DS socket service reports ready too early: the
 * numbers were noise, and Auto picked Texas from Europe (beta.13). */
#define REGIONS_VERSION 2
/* NVIDIA's own region for this address is kept unless another measures at
 * least this much faster: measurements wobble by a few ms. */
#define REGIONS_MARGIN_MS 15

static PlatLock g_lock = PLAT_LOCK_INIT;
static Region g_regions[REGION_MAX];
static unsigned g_count;
static char g_measured_ssid[40];
static int64_t g_measured_at;
static char g_choice[40];
/* NVIDIA's guess of the region for this internet address ("local-region"). */
static char g_local[40];
static char g_last_used[40];
static bool g_avoid_once;
/* The provider the cached list belongs to: a partner account (provider.h)
 * has its own regions, so another provider's list is dropped. */
static char g_provider[12];

/* Drop a list made for another provider. Call with the lock held. */
static void check_provider_locked(void)
{
    GfnProvider active;
    provider_active(&active);
    if (!strcmp(g_provider, active.code)) return;
    if (g_count || g_provider[0])
        diagnostic_log("REGION", "provider now %s (list was %s): regions cleared", active.code,
                       g_provider[0] ? g_provider : "-");
    g_count = 0;
    g_local[0] = '\0';
    g_measured_at = 0;
    g_measured_ssid[0] = '\0';
    snprintf(g_provider, sizeof(g_provider), "%s", active.code);
}

/* The label for the provider's own entry point. */
static const char *default_label(void)
{
    return provider_is_nvidia() ? "NVIDIA default" : "provider default";
}

void regions_avoid_once(void)
{
    plat_lock(&g_lock);
    g_avoid_once = true;
    plat_unlock(&g_lock);
}

void regions_last_used(char *name, size_t size)
{
    plat_lock(&g_lock);
    snprintf(name, size, "%s", g_last_used);
    plat_unlock(&g_lock);
}

static void set_last_used(const char *name)
{
    plat_lock(&g_lock);
    snprintf(g_last_used, sizeof(g_last_used), "%s", name);
    plat_unlock(&g_lock);
}

static void current_ssid(char *out, size_t size)
{
    plat_wifi_ssid(out, size);
}

static void save(void)
{
    json_t *list = json_array();
    plat_lock(&g_lock);
    for (unsigned i = 0; i < g_count; ++i)
        json_array_append_new(list, json_pack("{s:s,s:s,s:i}", "name", g_regions[i].name,
                                              "url", g_regions[i].url, "ms", g_regions[i].ms));
    json_t *root = json_pack("{s:i,s:o,s:s,s:s,s:I,s:s}", "version", REGIONS_VERSION, "regions", list,
                             "local", g_local, "ssid", g_measured_ssid,
                             "measured_at", (json_int_t)g_measured_at, "provider", g_provider);
    plat_unlock(&g_lock);
    if (root) json_dump_file(root, REGIONS_PATH, JSON_INDENT(1));
    json_decref(root);
}

void regions_load(void)
{
    plat_lock_init(&g_lock);
    json_error_t error;
    json_t *root = json_load_file(REGIONS_PATH, 0, &error);
    json_t *list = json_is_object(root) ? json_object_get(root, "regions") : NULL;
    json_t *version = json_is_object(root) ? json_object_get(root, "version") : NULL;
    /* Older measurements are not trusted (see REGIONS_VERSION). */
    const bool current = json_is_integer(version) && json_integer_value(version) == REGIONS_VERSION;
    size_t index;
    json_t *item;
    g_count = 0;
    json_array_foreach(list, index, item) {
        if (g_count >= REGION_MAX) break;
        const char *name = json_string_value(json_object_get(item, "name"));
        const char *url = json_string_value(json_object_get(item, "url"));
        json_t *ms = json_object_get(item, "ms");
        if (!name || !url || strncmp(url, "https://", 8)) continue;
        Region *r = &g_regions[g_count++];
        snprintf(r->name, sizeof(r->name), "%s", name);
        snprintf(r->url, sizeof(r->url), "%s", url);
        r->ms = current && json_is_integer(ms) ? (int)json_integer_value(ms) : -1;
    }
    const char *local = json_is_object(root) ? json_string_value(json_object_get(root, "local")) : NULL;
    snprintf(g_local, sizeof(g_local), "%s", local ? local : "");
    const char *ssid = json_is_object(root) ? json_string_value(json_object_get(root, "ssid")) : NULL;
    snprintf(g_measured_ssid, sizeof(g_measured_ssid), "%s", ssid ? ssid : "");
    json_t *at = json_is_object(root) ? json_object_get(root, "measured_at") : NULL;
    g_measured_at = current && json_is_integer(at) ? (int64_t)json_integer_value(at) : 0;
    /* Lists saved before providers existed are NVIDIA's. */
    const char *provider = json_is_object(root) ? json_string_value(json_object_get(root, "provider")) : NULL;
    snprintf(g_provider, sizeof(g_provider), "%s", provider && provider[0] ? provider : PROVIDER_NVIDIA);
    json_decref(root);
}

/* The list is in serverInfo's metaData: each region's name maps to its own
 * session service URL (besides "local-region" and "gfn-regions"). */
bool regions_update(void)
{
    static const char *const headers[] = { "Accept: application/json" };
    char base[96], url[128];
    provider_base_url(base, sizeof(base));
    snprintf(url, sizeof(url), "%s/v2/serverInfo", base);
    HttpResponse response;
    if (!http_request("GET", url, "Kasumi-3DS", headers, 1, NULL, 256 * 1024, &response)) {
        diagnostic_log("REGION", "list failed: %s", response.error);
        return false;
    }
    json_error_t error;
    json_t *root = response.status == 200 ? json_loadb(response.body ? response.body : "", response.size, 0, &error) : NULL;
    http_response_free(&response);
    json_t *meta = json_is_object(root) ? json_object_get(root, "metaData") : NULL;
    Region fresh[REGION_MAX];
    unsigned count = 0;
    char local[40] = "";
    size_t index;
    json_t *item;
    json_array_foreach(meta, index, item) {
        const char *key = json_string_value(json_object_get(item, "key"));
        const char *value = json_string_value(json_object_get(item, "value"));
        if (key && value && !strcmp(key, "local-region")) snprintf(local, sizeof(local), "%s", value);
        if (!key || !value || strncmp(value, "https://", 8) || !strstr(value, ".nvidiagrid.net") ||
            count >= REGION_MAX)
            continue;
        snprintf(fresh[count].name, sizeof(fresh[count].name), "%s", key);
        snprintf(fresh[count].url, sizeof(fresh[count].url), "%s", value);
        fresh[count].ms = -1;
        ++count;
    }
    json_decref(root);
    if (!count) {
        diagnostic_log("REGION", "list empty");
        return false;
    }
    plat_lock(&g_lock);
    check_provider_locked();
    /* Keep earlier measurements for regions that are still listed. */
    for (unsigned i = 0; i < count; ++i)
        for (unsigned j = 0; j < g_count; ++j)
            if (!strcmp(fresh[i].url, g_regions[j].url)) fresh[i].ms = g_regions[j].ms;
    memcpy(g_regions, fresh, sizeof(Region) * count);
    g_count = count;
    if (local[0]) snprintf(g_local, sizeof(g_local), "%s", local);
    plat_unlock(&g_lock);
    diagnostic_log("REGION", "list %u regions, NVIDIA's region for this address: %s", count,
                   local[0] ? local : "unknown");
    save();
    return true;
}

/* One region's round trip: a warm-up request opens the connection (TLS is
 * slow on a 3DS and must not be timed), then the faster of two requests on
 * that same connection. -1 if the region does not answer. */
static int ping_ms(const char *base)
{
    static const char *const headers[] = { "Accept: application/json" };
    char url[128];
    snprintf(url, sizeof(url), "%s/v2/serverInfo", base);
    HttpResponse response;
    http_next_request(5, NULL, NULL);
    const bool warm = http_request("GET", url, "Kasumi-3DS", headers, 1, NULL, 64 * 1024, &response);
    const long status = response.status;
    http_response_free(&response);
    if (!warm || status != 200) return -1;
    int best = -1;
    for (int i = 0; i < 2; ++i) {
        http_next_request(5, NULL, NULL);
        const u64 began = plat_time_ms();
        const bool ok = http_request("GET", url, "Kasumi-3DS", headers, 1, NULL, 64 * 1024, &response);
        const int ms = (int)(plat_time_ms() - began);
        const bool good = ok && response.status == 200;
        http_response_free(&response);
        if (good && (best < 0 || ms < best)) best = ms;
    }
    return best;
}

/* "eu", "us", "ca", "ap": the continent part of a region's host name. */
static void continent(const char *url, char *out, size_t size)
{
    const char *host = strncmp(url, "https://", 8) ? url : url + 8;
    size_t n = strcspn(host, "-.");
    if (n >= size) n = size - 1;
    memcpy(out, host, n);
    out[n] = '\0';
}

static bool same_area(const char *a, const char *b)
{
    /* North America is one area: US and Canadian regions sit side by side. */
    const bool a_na = !strcmp(a, "us") || !strcmp(a, "ca");
    const bool b_na = !strcmp(b, "us") || !strcmp(b, "ca");
    return a_na ? b_na : !strcmp(a, b);
}

void regions_measure(void)
{
    Region list[REGION_MAX];
    char local[40];
    plat_lock(&g_lock);
    const unsigned count = g_count;
    memcpy(list, g_regions, sizeof(Region) * count);
    snprintf(local, sizeof(local), "%s", g_local);
    plat_unlock(&g_lock);
    if (!count) return;
    /* Only the regions on NVIDIA's guessed continent: far ones cannot win,
     * and every region costs a TLS handshake. */
    char area[8] = "";
    for (unsigned i = 0; i < count; ++i)
        if (!strcmp(list[i].name, local)) continent(list[i].url, area, sizeof(area));
    unsigned measured = 0;
    for (unsigned i = 0; i < count; ++i) {
        char mine[8];
        continent(list[i].url, mine, sizeof(mine));
        if (area[0] && !same_area(area, mine)) {
            list[i].ms = -1;
            continue;
        }
        list[i].ms = ping_ms(list[i].url);
        ++measured;
        diagnostic_log("REGION", "ping %s %d ms", list[i].name, list[i].ms);
    }
    bool reached = false;
    for (unsigned i = 0; i < count; ++i) reached = reached || list[i].ms >= 0;
    plat_lock(&g_lock);
    for (unsigned i = 0; i < count && i < g_count; ++i)
        if (!strcmp(g_regions[i].url, list[i].url)) g_regions[i].ms = list[i].ms;
    /* Nothing answered (Wi-Fi dropped mid-way, beta.21 report 7QE6TZ): don't
     * keep that as this network's result for a week; measure again next time. */
    if (reached) {
        current_ssid(g_measured_ssid, sizeof(g_measured_ssid));
        g_measured_at = (int64_t)time(NULL);
    }
    plat_unlock(&g_lock);
    const int best = regions_fastest();
    Region chosen;
    if (best >= 0 && regions_get((unsigned)best, &chosen))
        diagnostic_log("REGION", "measured %u of %u regions (area %s): fastest %s %d ms", measured, count,
                       area[0] ? area : "all", chosen.name, chosen.ms);
    else
        diagnostic_log("REGION", "measured %u of %u regions: none reachable", measured, count);
    save();
}

unsigned regions_count(void)
{
    plat_lock(&g_lock);
    check_provider_locked();
    const unsigned count = g_count;
    plat_unlock(&g_lock);
    return count;
}

bool regions_get(unsigned index, Region *out)
{
    plat_lock(&g_lock);
    const bool ok = index < g_count;
    if (ok) *out = g_regions[index];
    plat_unlock(&g_lock);
    return ok;
}

int regions_fastest(void)
{
    int best = -1, local = -1;
    plat_lock(&g_lock);
    for (unsigned i = 0; i < g_count; ++i) {
        if (g_regions[i].ms < 0) continue;
        if (!strcmp(g_regions[i].name, g_local)) local = (int)i;
        if (best < 0 || g_regions[i].ms < g_regions[best].ms) best = (int)i;
    }
    if (local >= 0 && best >= 0 && g_regions[local].ms <= g_regions[best].ms + REGIONS_MARGIN_MS) best = local;
    plat_unlock(&g_lock);
    return best;
}

bool regions_measured_here(void)
{
    char ssid[40];
    current_ssid(ssid, sizeof(ssid));
    plat_lock(&g_lock);
    const int64_t age = (int64_t)time(NULL) - g_measured_at;
    const bool here = g_measured_at && age >= 0 && age < REGIONS_MAX_AGE && !strcmp(ssid, g_measured_ssid);
    plat_unlock(&g_lock);
    return here;
}

void regions_set_choice(const char *choice)
{
    plat_lock(&g_lock);
    snprintf(g_choice, sizeof(g_choice), "%s", choice ? choice : "");
    plat_unlock(&g_lock);
}

void regions_resolve(char *url, size_t size)
{
    char choice[40];
    plat_lock(&g_lock);
    snprintf(choice, sizeof(choice), "%s", g_choice);
    plat_unlock(&g_lock);
    provider_base_url(url, size);
    if (!strcmp(choice, REGION_CHOICE_NVIDIA)) {
        diagnostic_log("REGION", "server=%s", default_label());
        set_last_used(default_label());
        return;
    }
    if (choice[0]) {
        plat_lock(&g_lock);
        bool found = false;
        for (unsigned i = 0; i < g_count && !found; ++i) {
            if (strcmp(g_regions[i].name, choice)) continue;
            snprintf(url, size, "%s", g_regions[i].url);
            found = true;
        }
        plat_unlock(&g_lock);
        diagnostic_log("REGION", "server=%s%s", choice, found ? "" : " (not listed; default)");
        set_last_used(found ? choice : default_label());
        return;
    }
    plat_lock(&g_lock);
    const bool avoid = g_avoid_once;
    g_avoid_once = false;
    plat_unlock(&g_lock);
    if (avoid) {
        diagnostic_log("REGION", "server=auto: last session there failed; %s this time", default_label());
        set_last_used(default_label());
        return;
    }
    /* Auto: the region with the lowest ping from this network. Measuring
     * matters most on mobile data, whose address can make NVIDIA pick a
     * far-away region. */
    if (!regions_count()) regions_update();
    if (!regions_measured_here()) regions_measure();
    const int best = regions_fastest();
    Region region;
    char local[40];
    plat_lock(&g_lock);
    snprintf(local, sizeof(local), "%s", g_local);
    plat_unlock(&g_lock);
    if (best >= 0 && regions_get((unsigned)best, &region) && !strcmp(region.name, local)) {
        /* NVIDIA's own region is the fastest: go through NVIDIA's normal
         * address, which balances the queue across its servers. Queued
         * sessions made straight at the regional address sat frozen for two
         * minutes and were dropped as SESSION_REQUEST_IN_QUEUE_ABANDONED
         * (beta.17: Germany at place 26, N. California at 21), while the
         * retry through the normal address moved and streamed. */
        diagnostic_log("REGION", "server=auto NVIDIA's pick %s is fastest (%d ms)", region.name, region.ms);
        set_last_used(region.name);
    } else if (best >= 0 && regions_get((unsigned)best, &region)) {
        snprintf(url, size, "%s", region.url);
        diagnostic_log("REGION", "server=auto %s %d ms (NVIDIA's pick: %s)", region.name, region.ms,
                       local[0] ? local : "unknown");
        set_last_used(region.name);
    } else {
        diagnostic_log("REGION", "server=auto: no measurement; %s", default_label());
        set_last_used(default_label());
    }
}
