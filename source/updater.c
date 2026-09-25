#include "updater.h"

#include <3ds.h>
#include <jansson.h>
#include <limits.h>
#include <stdarg.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "app_paths.h"
#include "diagnostic.h"
#include "http_client.h"

#define RELEASES_URL "https://api.github.com/repos/" APP_REPOSITORY "/releases?per_page=10"
#define STATE_PATH APP_DATA_DIR "/update.json"
#define DOWNLOAD_PATH APP_DATA_DIR "/update.download"
#define USER_AGENT APP_NAME "-3DS/" APP_VERSION
#define MAX_PACKAGE (32u * 1024u * 1024u)

static LightLock g_lock = 1;
static UpdateInfo g_info;
static char g_self_path[256];
static char g_package_url[512];
static char g_sums_url[512];
static char g_package_name[32];
static char g_dismissed[32];
static bool g_installed_cia;

/* ---- State file ----------------------------------------------------------- */

static void save_state(const char *whats_new_version, const char *whats_new_notes)
{
    json_error_t error;
    json_t *root = json_load_file(STATE_PATH, 0, &error);
    if (!json_is_object(root)) {
        json_decref(root);
        root = json_object();
    }
    json_object_set_new(root, "checked_at", json_integer((json_int_t)g_info.checked_at));
    json_object_set_new(root, "dismissed", json_string(g_dismissed));
    if (whats_new_version)
        json_object_set_new(root, "whats_new", json_pack("{s:s,s:s}", "version", whats_new_version,
                                                         "notes", whats_new_notes ? whats_new_notes : ""));
    json_dump_file(root, STATE_PATH, JSON_COMPACT);
    json_decref(root);
}

void updater_init(const char *self_path)
{
    LightLock_Init(&g_lock);
    memset(&g_info, 0, sizeof(g_info));
    snprintf(g_self_path, sizeof(g_self_path), "%s", self_path ? self_path : "");
    json_error_t error;
    json_t *root = json_load_file(STATE_PATH, 0, &error);
    if (json_is_object(root)) {
        json_t *checked = json_object_get(root, "checked_at");
        if (json_is_integer(checked)) g_info.checked_at = json_integer_value(checked);
        json_t *dismissed = json_object_get(root, "dismissed");
        if (json_is_string(dismissed)) snprintf(g_dismissed, sizeof(g_dismissed), "%s", json_string_value(dismissed));
    }
    json_decref(root);
    remove(DOWNLOAD_PATH);
}

UpdateInfo updater_info(void)
{
    LightLock_Lock(&g_lock);
    const UpdateInfo copy = g_info;
    LightLock_Unlock(&g_lock);
    return copy;
}

static void set_state(UpdateState state, unsigned progress)
{
    LightLock_Lock(&g_lock);
    g_info.state = state;
    g_info.progress = progress;
    LightLock_Unlock(&g_lock);
}

static bool fail(const char *format, ...) __attribute__((format(printf, 1, 2)));
static bool fail(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    LightLock_Lock(&g_lock);
    vsnprintf(g_info.error, sizeof(g_info.error), format, args);
    g_info.state = UPDATE_FAILED;
    LightLock_Unlock(&g_lock);
    va_end(args);
    diagnostic_log("UPDATE", "failed: %s", g_info.error);
    remove(DOWNLOAD_PATH);
    return false;
}

bool updater_is_3dsx(void) { return envIsHomebrew(); }

bool updater_check_due(void)
{
    const int64_t now = (int64_t)time(NULL);
    return now - g_info.checked_at >= 12 * 3600 || now < g_info.checked_at;
}

/* ---- Versions -------------------------------------------------------------- */

/* "v1.2.3", "1.2.3-beta.4": a release sorts after every pre-release of it. */
static bool parse_version(const char *text, long parts[4])
{
    if (!text) return false;
    if (*text == 'v' || *text == 'V') ++text;
    char *end;
    for (int i = 0; i < 3; ++i) {
        parts[i] = strtol(text, &end, 10);
        if (end == text) return false;
        text = end;
        if (i < 2) {
            if (*text != '.') return false;
            ++text;
        }
    }
    parts[3] = LONG_MAX;
    if (*text == '-') {
        const char *digits = text;
        while (*digits && (*digits < '0' || *digits > '9')) ++digits;
        parts[3] = *digits ? strtol(digits, NULL, 10) : 0;
    }
    return true;
}

static int compare_versions(const long a[4], const long b[4])
{
    for (int i = 0; i < 4; ++i)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

/* ---- Check ---------------------------------------------------------------- */

static const char *const API_HEADERS[] = {
    "Accept: application/vnd.github+json", "X-GitHub-Api-Version: 2022-11-28"
};

/* Release notes are Markdown; keep them readable as plain text on the 3DS. */
static void plain_notes(char *out, size_t size, const char *markdown)
{
    size_t n = 0;
    bool line_start = true;
    for (const char *p = markdown; *p && n + 4 < size; ++p) {
        if (*p == '\r') continue;
        if (line_start && (*p == '#' || *p == '>')) {
            while (*p == '#' || *p == '>' || *p == ' ') ++p;
            if (!*p) break;
        }
        if (line_start && (*p == '-' || *p == '*') && p[1] == ' ') {
            memcpy(out + n, "- ", 2);
            n += 2;
            ++p;
            line_start = false;
            continue;
        }
        if (*p == '*' || *p == '`' || *p == '_') continue;
        out[n++] = *p;
        line_start = *p == '\n';
    }
    while (n && (out[n - 1] == '\n' || out[n - 1] == ' ')) --n;
    out[n] = '\0';
}

bool updater_check(bool include_beta)
{
    set_state(UPDATE_CHECKING, 0);
    HttpResponse response;
    if (!http_request("GET", RELEASES_URL, USER_AGENT, API_HEADERS, 2, NULL, 512 * 1024, &response))
        return fail("Could not reach GitHub (%s)", response.error);
    const long status = response.status;
    json_error_t error;
    json_t *root = response.body ? json_loadb(response.body, response.size, 0, &error) : NULL;
    http_response_free(&response);
    if (status == 403) {
        json_decref(root);
        return fail("GitHub is rate limiting checks; try again later");
    }
    if (status != 200 || !json_is_array(root)) {
        json_decref(root);
        return fail("GitHub answered HTTP %ld", status);
    }

    long current[4];
    parse_version(APP_VERSION, current);
    json_t *best = NULL;
    long best_version[4] = { -1, -1, -1, -1 };
    size_t index; json_t *release;
    json_array_foreach(root, index, release) {
        if (json_is_true(json_object_get(release, "draft"))) continue;
        const bool pre = json_is_true(json_object_get(release, "prerelease"));
        if (pre && !include_beta) continue;
        long version[4];
        if (!parse_version(json_string_value(json_object_get(release, "tag_name")), version)) continue;
        if (compare_versions(version, best_version) > 0) {
            best = release;
            memcpy(best_version, version, sizeof(version));
        }
    }

    const char *const wanted = updater_is_3dsx() ? "Kasumi.3dsx" : "Kasumi.cia";
    LightLock_Lock(&g_lock);
    g_info.checked_at = (int64_t)time(NULL);
    g_info.error[0] = '\0';
    bool available = false;
    if (best && compare_versions(best_version, current) > 0) {
        const char *tag = json_string_value(json_object_get(best, "tag_name"));
        snprintf(g_info.latest, sizeof(g_info.latest), "%s", tag[0] == 'v' ? tag + 1 : tag);
        g_info.prerelease = json_is_true(json_object_get(best, "prerelease"));
        const char *published = json_string_value(json_object_get(best, "published_at"));
        snprintf(g_info.published, sizeof(g_info.published), "%.10s", published ? published : "");
        const char *body = json_string_value(json_object_get(best, "body"));
        plain_notes(g_info.notes, sizeof(g_info.notes), body ? body : "");
        g_package_url[0] = g_sums_url[0] = '\0';
        g_info.size_bytes = 0;
        json_t *assets = json_object_get(best, "assets");
        size_t a; json_t *asset;
        json_array_foreach(assets, a, asset) {
            const char *name = json_string_value(json_object_get(asset, "name"));
            const char *url = json_string_value(json_object_get(asset, "browser_download_url"));
            if (!name || !url) continue;
            if (!strcmp(name, wanted)) {
                snprintf(g_package_url, sizeof(g_package_url), "%s", url);
                snprintf(g_package_name, sizeof(g_package_name), "%s", name);
                json_t *size = json_object_get(asset, "size");
                g_info.size_bytes = json_is_integer(size) ? (unsigned long)json_integer_value(size) : 0;
            } else if (!strcmp(name, "SHA256SUMS")) {
                snprintf(g_sums_url, sizeof(g_sums_url), "%s", url);
            }
        }
        available = g_package_url[0] && g_sums_url[0];
        if (!available)
            snprintf(g_info.error, sizeof(g_info.error), "Release %s has no %s with checksums", g_info.latest, wanted);
    }
    g_info.state = available ? UPDATE_AVAILABLE : UPDATE_UP_TO_DATE;
    g_info.progress = 0;
    LightLock_Unlock(&g_lock);
    json_decref(root);
    save_state(NULL, NULL);
    diagnostic_log("UPDATE", "check current=%s latest=%s available=%d beta=%d",
                   APP_VERSION, available ? g_info.latest : "-", available, include_beta);
    return true;
}

/* ---- Install ---------------------------------------------------------------- */

static void on_download(unsigned long long received, unsigned long long total, void *context)
{
    (void)context;
    const unsigned long long expected = total ? total : g_info.size_bytes;
    if (!expected) return;
    unsigned progress = (unsigned)(received * 1000 / expected);
    set_state(UPDATE_DOWNLOADING, progress > 1000 ? 1000 : progress);
}

static bool expected_hash(const char *sums, const char *name, unsigned char out[32])
{
    const char *line = sums;
    while (line && *line) {
        const char *end = strchr(line, '\n');
        const size_t length = end ? (size_t)(end - line) : strlen(line);
        /* "<64 hex>  <name>" or "<64 hex> *<name>" */
        if (length > 66) {
            const char *file = line + 64;
            while (*file == ' ' || *file == '*') ++file;
            const size_t file_length = length - (size_t)(file - line);
            if (file_length >= strlen(name) && !strncmp(file, name, strlen(name))) {
                for (int i = 0; i < 32; ++i) {
                    unsigned value;
                    if (sscanf(line + i * 2, "%2x", &value) != 1) return false;
                    out[i] = (unsigned char)value;
                }
                return true;
            }
        }
        line = end ? end + 1 : NULL;
    }
    return false;
}

static bool battery_ok(char *why, size_t size)
{
    u8 level = 5, charging = 0;
    if (R_SUCCEEDED(ptmuInit())) {
        PTMU_GetBatteryLevel(&level);
        PTMU_GetBatteryChargeState(&charging);
        ptmuExit();
    }
    if (level >= 2 || charging) return true;
    snprintf(why, size, "Battery too low: charge the console first");
    return false;
}

static bool sd_space_ok(unsigned long needed, char *why, size_t size)
{
    FS_ArchiveResource resource;
    if (R_FAILED(FSUSER_GetSdmcArchiveResource(&resource))) return true;
    const unsigned long long free_bytes = (unsigned long long)resource.freeClusters * resource.clusterSize;
    if (free_bytes >= (unsigned long long)needed * 3) return true;
    snprintf(why, size, "Not enough SD card space (%llu MB free)", free_bytes / (1024 * 1024));
    return false;
}

static bool install_cia(const unsigned char *data, size_t size)
{
    if (R_FAILED(amInit())) return fail("Could not open the system installer (am)");
    Handle cia;
    Result rc = AM_StartCiaInstall(MEDIATYPE_SD, &cia);
    if (R_FAILED(rc)) {
        amExit();
        return fail("Installer refused to start (0x%08lX)", (unsigned long)rc);
    }
    const size_t chunk = 128 * 1024;
    for (size_t offset = 0; offset < size; offset += chunk) {
        const size_t n = size - offset < chunk ? size - offset : chunk;
        u32 written = 0;
        rc = FSFILE_Write(cia, &written, offset, data + offset, (u32)n, 0);
        if (R_FAILED(rc) || written != n) {
            AM_CancelCIAInstall(cia);
            amExit();
            return fail("Install write failed (0x%08lX); the old version is untouched", (unsigned long)rc);
        }
        set_state(UPDATE_INSTALLING, (unsigned)((offset + n) * 1000 / size));
    }
    rc = AM_FinishCiaInstall(cia);
    amExit();
    if (R_FAILED(rc)) return fail("Install could not finish (0x%08lX)", (unsigned long)rc);
    g_installed_cia = true;
    return true;
}

static bool install_3dsx(const unsigned char *data, size_t size)
{
    if (!g_self_path[0] || strncmp(g_self_path, "sdmc:/", 6))
        return fail("Could not tell where Kasumi.3dsx is; update it by hand");
    char next[300], old[300];
    snprintf(next, sizeof(next), "%s.new", g_self_path);
    snprintf(old, sizeof(old), "%s.old", g_self_path);
    FILE *f = fopen(next, "wb");
    if (!f) return fail("Could not write %s", next);
    const bool written = fwrite(data, 1, size, f) == size;
    fclose(f);
    if (!written) { remove(next); return fail("SD card write failed"); }
    set_state(UPDATE_INSTALLING, 500);
    /* Keep the old file until the new one is in place. */
    remove(old);
    if (rename(g_self_path, old) != 0) { remove(next); return fail("Could not replace Kasumi.3dsx"); }
    if (rename(next, g_self_path) != 0) {
        rename(old, g_self_path);
        return fail("Could not replace Kasumi.3dsx");
    }
    remove(old);
    set_state(UPDATE_INSTALLING, 1000);
    return true;
}

bool updater_install(void)
{
    const UpdateInfo info = updater_info();
    if (info.state != UPDATE_AVAILABLE && info.state != UPDATE_FAILED) return false;
    if (!g_package_url[0] || !g_sums_url[0]) return fail("Check for updates first");
    char why[96];
    if (!battery_ok(why, sizeof(why))) return fail("%s", why);
    if (!sd_space_ok(info.size_bytes ? info.size_bytes : 4u * 1024u * 1024u, why, sizeof(why)))
        return fail("%s", why);

    /* Checksums first: tiny, and a release without them is never installed. */
    set_state(UPDATE_DOWNLOADING, 0);
    HttpResponse sums;
    if (!http_request("GET", g_sums_url, USER_AGENT, NULL, 0, NULL, 64 * 1024, &sums) || sums.status != 200) {
        http_response_free(&sums);
        return fail("Could not download the release checksums");
    }
    unsigned char want[32];
    const bool have_hash = sums.body && expected_hash(sums.body, g_package_name, want);
    http_response_free(&sums);
    if (!have_hash) return fail("The release checksums do not list %s", g_package_name);

    HttpResponse package;
    http_next_request(300, on_download, NULL);
    if (!http_request("GET", g_package_url, USER_AGENT, NULL, 0, NULL, MAX_PACKAGE, &package) ||
        package.status != 200 || !package.body || package.size < 1024) {
        const long status = package.status;
        http_response_free(&package);
        return fail("Download failed (HTTP %ld)", status);
    }

    set_state(UPDATE_VERIFYING, 0);
    unsigned char got[32];
    mbedtls_sha256_ret((const unsigned char *)package.body, package.size, got, 0);
    if (memcmp(got, want, sizeof(got)) != 0) {
        http_response_free(&package);
        return fail("Download is damaged (checksum mismatch); nothing was installed");
    }
    set_state(UPDATE_VERIFYING, 1000);
    diagnostic_log("UPDATE", "downloaded %s %lu bytes, checksum ok", g_package_name,
                   (unsigned long)package.size);

    set_state(UPDATE_INSTALLING, 0);
    const bool ok = updater_is_3dsx() ? install_3dsx((const unsigned char *)package.body, package.size)
                                      : install_cia((const unsigned char *)package.body, package.size);
    http_response_free(&package);
    if (!ok) return false;
    /* The new version shows its notes once on its first start. */
    save_state(info.latest, info.notes);
    set_state(UPDATE_INSTALLED, 1000);
    diagnostic_log("UPDATE", "installed %s", info.latest);
    return true;
}

/* ---- After install --------------------------------------------------------- */

bool updater_can_relaunch(void) { return g_installed_cia; }

void updater_relaunch(void)
{
    /* On exit, the system starts this title again: the new version. */
    if (g_installed_cia) aptSetChainloaderToSelf();
}

bool updater_take_whats_new(char *version, unsigned version_size, char *notes, unsigned notes_size)
{
    json_error_t error;
    json_t *root = json_load_file(STATE_PATH, 0, &error);
    json_t *whats_new = json_is_object(root) ? json_object_get(root, "whats_new") : NULL;
    const char *v = json_is_object(whats_new) ? json_string_value(json_object_get(whats_new, "version")) : NULL;
    bool show = false;
    if (v && !strcmp(v, APP_VERSION)) {
        const char *n = json_string_value(json_object_get(whats_new, "notes"));
        snprintf(version, version_size, "%s", v);
        snprintf(notes, notes_size, "%s", n ? n : "");
        show = true;
    }
    if (whats_new) {
        json_object_del(root, "whats_new");
        json_dump_file(root, STATE_PATH, JSON_COMPACT);
    }
    json_decref(root);
    return show;
}

void updater_dismiss(void)
{
    snprintf(g_dismissed, sizeof(g_dismissed), "%s", g_info.latest);
    save_state(NULL, NULL);
}

bool updater_dismissed(void)
{
    return g_info.latest[0] && !strcmp(g_dismissed, g_info.latest);
}
