#include "steam_link.h"

#include <3ds.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "app_paths.h"
#include "diagnostic.h"
#include "http_client.h"
#include "provider.h"
#include "steam_crypto.h"
#include "steam_remote.h"
#include "steam_udp.h"
#include "stream_profile.h"

#define SL_PATH APP_DATA_DIR "/steam-link.json"
#define SL_RECENT_MAX 32
/* Paired PCs kept (the one in use first). */
#define SL_PC_MAX 4
/* The library's own entries (app_id); anything else is a Steam game id. */
#define SL_BIG_PICTURE "steam:bigpicture"
#define SL_DESKTOP "steam:desktop"

typedef struct {
    uint64_t gameid;
    char name[96];
} Recent;

/* A paired PC other than the one in use, with its own played games. */
typedef struct {
    SteamHost host;
    uint8_t secret[32];
    Recent recent[SL_RECENT_MAX];
    unsigned recent_count;
} OtherPc;

static struct {
    bool loaded;
    SteamIdentity id;
    bool paired;
    SteamHost host;          /* the paired PC: name, last address, client id, user */
    uint8_t host_secret[32];
    Recent recent[SL_RECENT_MAX];
    unsigned recent_count;
    bool recent_dirty;
    OtherPc others[SL_PC_MAX - 1];
    unsigned other_count;
    /* Sign-in in progress. */
    bool pairing;
    SteamPairing pair;
    /* The stream the PC granted, for steam_link_signal_start. */
    SteamStreamGrant grant;
} g;

static LightLock g_lock;
static volatile bool g_selected;
/* B while a stream is being asked for (net_worker_cancel). */
static volatile bool g_cancel;

void steam_link_cancel(void) { g_cancel = true; }
static char g_host_name[64];
/* The paired PCs' names for the screens (the one in use first). */
static char g_pc_names[SL_PC_MAX][64];
static unsigned g_pc_count;

void steam_link_select(bool steam)
{
    static bool once;
    if (!once) {
        LightLock_Init(&g_lock);
        /* Builds the checksum table before any second thread needs it. */
        steam_crc32c("", 0);
        once = true;
    }
    g_selected = steam;
}

bool steam_link_selected(void) { return g_selected; }

const char *steam_link_host_name(void) { return g_host_name; }

bool steam_link_saved_pc(char *name, size_t size)
{
    json_error_t error;
    json_t *root = json_load_file(SL_PATH, 0, &error);
    /* "hosts" (the one in use first), or the single "host" of build 126. */
    json_t *host = json_array_get(json_object_get(root, "hosts"), 0);
    if (!host) host = json_object_get(root, "host");
    const char *pc = json_string_value(json_object_get(host, "name"));
    const bool paired = json_is_object(host) && pc;
    if (size) snprintf(name, size, "%s", paired ? pc : "");
    json_decref(root);
    return paired;
}

unsigned steam_link_pcs(char names[][64], unsigned max)
{
    LightLock_Lock(&g_lock);
    const unsigned count = g_pc_count < max ? g_pc_count : max;
    for (unsigned i = 0; i < count; ++i) memcpy(names[i], g_pc_names[i], sizeof(g_pc_names[i]));
    LightLock_Unlock(&g_lock);
    return count;
}

/* ---- The saved pairing ---------------------------------------------------------- */

static void hex(const uint8_t *data, size_t size, char *out)
{
    for (size_t i = 0; i < size; ++i) snprintf(out + 2 * i, 3, "%02x", data[i]);
}

static bool unhex(const char *text, uint8_t *out, size_t size)
{
    if (!text || strlen(text) != size * 2) return false;
    for (size_t i = 0; i < size; ++i) {
        unsigned v;
        if (sscanf(text + 2 * i, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

static const char *text_of(json_t *object, const char *key)
{
    const char *value = json_string_value(json_object_get(object, key));
    return value ? value : "";
}

static uint64_t u64_of(json_t *object, const char *key)
{
    return strtoull(text_of(object, key), NULL, 10);
}

/* The names the screens show, after any change to the paired PCs. */
static void publish_pcs(void)
{
    LightLock_Lock(&g_lock);
    snprintf(g_host_name, sizeof(g_host_name), "%s", g.paired ? g.host.name : "");
    g_pc_count = 0;
    if (g.paired) snprintf(g_pc_names[g_pc_count++], 64, "%s", g.host.name);
    for (unsigned i = 0; i < g.other_count && g_pc_count < SL_PC_MAX; ++i)
        snprintf(g_pc_names[g_pc_count++], 64, "%s", g.others[i].host.name);
    LightLock_Unlock(&g_lock);
}

static json_t *pc_json(const SteamHost *host, const uint8_t secret[32], const Recent *recent, unsigned count)
{
    char hex_secret[65], client[24], steamid[24], ip[20];
    hex(secret, 32, hex_secret);
    snprintf(client, sizeof(client), "%llu", (unsigned long long)host->client_id);
    snprintf(steamid, sizeof(steamid), "%llu", (unsigned long long)host->steamid);
    steam_ip_format(host->ip, ip, sizeof(ip));
    json_t *games = json_array();
    for (unsigned i = 0; i < count; ++i) {
        char game[24];
        snprintf(game, sizeof(game), "%llu", (unsigned long long)recent[i].gameid);
        json_array_append_new(games, json_pack("{s:s,s:s}", "id", game, "name", recent[i].name));
    }
    return json_pack("{s:s,s:s,s:s,s:s,s:s,s:o}", "name", host->name, "ip", ip, "client_id", client,
                     "steamid", steamid, "secret", hex_secret, "recent", games);
}

static bool pc_from_json(json_t *object, SteamHost *host, uint8_t secret[32], Recent *recent, unsigned *count,
                         json_t *legacy_recent)
{
    if (!json_is_object(object) || !unhex(text_of(object, "secret"), secret, 32)) return false;
    memset(host, 0, sizeof(*host));
    snprintf(host->name, sizeof(host->name), "%s", text_of(object, "name"));
    host->ip = steam_ip_parse(text_of(object, "ip"));
    host->client_id = u64_of(object, "client_id");
    host->steamid = u64_of(object, "steamid");
    json_t *games = json_object_get(object, "recent");
    if (!games) games = legacy_recent;
    size_t index;
    json_t *item;
    *count = 0;
    json_array_foreach(games, index, item) {
        if (*count >= SL_RECENT_MAX) break;
        Recent *r = &recent[*count];
        r->gameid = u64_of(item, "id");
        snprintf(r->name, sizeof(r->name), "%s", text_of(item, "name"));
        if (r->gameid) ++*count;
    }
    return host->client_id != 0;
}

static void save(void)
{
    char secret[65], id[24];
    hex(g.id.secret, 32, secret);
    snprintf(id, sizeof(id), "%llu", (unsigned long long)g.id.device_id);
    json_t *root = json_pack("{s:s,s:s}", "device_id", id, "device_secret", secret);
    if (!root) return;
    json_t *hosts = json_array();
    LightLock_Lock(&g_lock);
    if (g.paired) json_array_append_new(hosts, pc_json(&g.host, g.host_secret, g.recent, g.recent_count));
    for (unsigned i = 0; i < g.other_count; ++i)
        json_array_append_new(hosts, pc_json(&g.others[i].host, g.others[i].secret, g.others[i].recent,
                                             g.others[i].recent_count));
    g.recent_dirty = false;
    LightLock_Unlock(&g_lock);
    json_object_set_new(root, "hosts", hosts);
    if (json_dump_file(root, SL_PATH, JSON_COMPACT) != 0) diagnostic_log("STEAM", "saving the pairing failed");
    json_decref(root);
}

static void load(void)
{
    if (g.loaded) return;
    g.loaded = true;
    json_error_t error;
    json_t *root = json_load_file(SL_PATH, 0, &error);
    if (json_is_object(root)) {
        g.id.device_id = u64_of(root, "device_id");
        unhex(text_of(root, "device_secret"), g.id.secret, 32);
        json_t *hosts = json_object_get(root, "hosts");
        LightLock_Lock(&g_lock);
        g.paired = false;
        g.other_count = 0;
        if (json_is_array(hosts)) {
            size_t index;
            json_t *item;
            json_array_foreach(hosts, index, item) {
                if (!g.paired) {
                    g.paired = pc_from_json(item, &g.host, g.host_secret, g.recent, &g.recent_count, NULL);
                } else if (g.other_count < SL_PC_MAX - 1) {
                    OtherPc *o = &g.others[g.other_count];
                    if (pc_from_json(item, &o->host, o->secret, o->recent, &o->recent_count, NULL)) ++g.other_count;
                }
            }
        } else {
            /* Build 126: one "host", its games in "recent". */
            g.paired = pc_from_json(json_object_get(root, "host"), &g.host, g.host_secret, g.recent,
                                    &g.recent_count, json_object_get(root, "recent"));
        }
        LightLock_Unlock(&g_lock);
    }
    json_decref(root);
    snprintf(g.id.name, sizeof(g.id.name), "Kasumi (New 3DS)");
    if (!g.id.device_id) {
        /* This console's identity as a Steam Link device, made once. */
        while (!g.id.device_id) steam_random(&g.id.device_id, sizeof(g.id.device_id));
        g.id.device_id &= 0x7fffffffffffffffull;
        steam_random(g.id.secret, sizeof(g.id.secret));
        save();
    }
    publish_pcs();
}

static void provider_steam(GfnProvider *out)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->code, sizeof(out->code), "%s", PROVIDER_STEAM);
    snprintf(out->name, sizeof(out->name), "Steam Link");
}

static void fill_tokens(GfnClient *c)
{
    /* Placeholders: the screens read "signed in" from these. */
    snprintf(c->access_token, sizeof(c->access_token), "steam-link");
    snprintf(c->refresh_token, sizeof(c->refresh_token), "steam-link");
    snprintf(c->user_id, sizeof(c->user_id), "%llu", (unsigned long long)g.host.steamid);
    c->token_expires_at = c->client_token_expires_at = INT64_MAX / 2;
    provider_steam(&c->login_provider);
}

bool steam_link_load_login(GfnClient *c)
{
    g.loaded = false;
    load();
    if (!g.paired) return false;
    fill_tokens(c);
    return true;
}

/* Drops one of the other PCs. */
static void drop_other(unsigned index)
{
    LightLock_Lock(&g_lock);
    memmove(&g.others[index], &g.others[index + 1], (g.other_count - index - 1) * sizeof(OtherPc));
    --g.other_count;
    LightLock_Unlock(&g_lock);
}

/* One of the other PCs becomes the one in use (whatever was in use is
 * overwritten: push_current first to keep it). */
static void take_other(unsigned index)
{
    LightLock_Lock(&g_lock);
    const OtherPc next = g.others[index];
    memmove(&g.others[index], &g.others[index + 1], (g.other_count - index - 1) * sizeof(OtherPc));
    --g.other_count;
    g.host = next.host;
    memcpy(g.host_secret, next.secret, sizeof(g.host_secret));
    memcpy(g.recent, next.recent, sizeof(g.recent));
    g.recent_count = next.recent_count;
    g.paired = true;
    LightLock_Unlock(&g_lock);
}

/* The PC in use moves to the front of the others (room is made). */
static void push_current(void)
{
    if (!g.paired) return;
    LightLock_Lock(&g_lock);
    if (g.other_count == SL_PC_MAX - 1) --g.other_count;
    memmove(&g.others[1], &g.others[0], g.other_count * sizeof(OtherPc));
    OtherPc *o = &g.others[0];
    o->host = g.host;
    memcpy(o->secret, g.host_secret, sizeof(o->secret));
    memcpy(o->recent, g.recent, sizeof(o->recent));
    o->recent_count = g.recent_count;
    ++g.other_count;
    g.paired = false;
    g.recent_count = 0;
    LightLock_Unlock(&g_lock);
}

bool steam_link_sign_out(GfnClient *c)
{
    if (g.pairing) steam_pair_cancel(&g.pair);
    g.pairing = false;
    load();
    /* Forget the PC in use; another paired one takes its place. The device
     * identity stays: PCs list this console by it. */
    LightLock_Lock(&g_lock);
    g.paired = false;
    memset(&g.host, 0, sizeof(g.host));
    memset(g.host_secret, 0, sizeof(g.host_secret));
    g.recent_count = 0;
    LightLock_Unlock(&g_lock);
    if (g.other_count) take_other(0);
    save();
    publish_pcs();
    if (g.paired && c) fill_tokens(c);
    return g.paired;
}

bool steam_link_use_pc(GfnClient *c, unsigned index)
{
    load();
    if (!index || index > g.other_count) return g.paired;
    const unsigned other = index - 1;
    push_current();
    /* push_current moved every other PC one place down. */
    take_other(other + 1);
    save();
    publish_pcs();
    fill_tokens(c);
    diagnostic_log("STEAM", "now streaming from another paired PC");
    return gfn_fetch_library(c);
}

/* ---- Pairing (the sign-in) ------------------------------------------------------- */

bool steam_link_begin_login(GfnClient *c)
{
    load();
    if (g.pairing) steam_pair_cancel(&g.pair);
    g.pairing = false;
    provider_steam(&c->login_provider);
    snprintf(c->status, sizeof(c->status), "Looking for a PC running Steam on this Wi-Fi...");
    SteamHost hosts[4];
    const int found = steam_discover(&g.id, 0, hosts, 4, 2000);
    if (!found && g.paired) {
        /* Pairing another PC: stay on the one paired before. */
        c->auth_state = GFN_AUTH_LOGGED_IN;
        snprintf(c->status, sizeof(c->status), "No other PC with Steam found on this Wi-Fi.");
        return false;
    }
    if (!found) {
        c->auth_state = GFN_AUTH_ERROR;
        snprintf(c->status, sizeof(c->status), "No PC with Steam found on this Wi-Fi. Start Steam on your PC, "
                 "check Remote Play is on in its settings, then press X.");
        return false;
    }
    /* A PC not paired yet first (pairing another), then one with someone
     * signed in to Steam. */
    int pick = -1;
    for (int pass = 0; pass < 3 && pick < 0; ++pass)
        for (int i = 0; i < found && pick < 0; ++i) {
            bool known = g.paired && hosts[i].client_id == g.host.client_id;
            for (unsigned k = 0; k < g.other_count; ++k) known |= hosts[i].client_id == g.others[k].host.client_id;
            if ((pass == 0 && !known && hosts[i].steamid) || (pass == 1 && !known) || pass == 2) pick = i;
        }
    uint16_t random_pin = 0;
    steam_random(&random_pin, sizeof(random_pin));
    char pin[8];
    snprintf(pin, sizeof(pin), "%04u", (unsigned)(random_pin % 10000));
    if (!steam_pair_begin(&g.pair, &g.id, &hosts[pick], pin)) {
        c->auth_state = GFN_AUTH_ERROR;
        snprintf(c->status, sizeof(c->status), "Couldn't prepare the pairing. Press X to try again.");
        return false;
    }
    g.pairing = true;
    snprintf(c->user_code, sizeof(c->user_code), "%s", pin);
    snprintf(c->verification_uri, sizeof(c->verification_uri), "%.63s", hosts[pick].name);
    const int64_t now = (int64_t)time(NULL);
    c->challenge_expires_at = now + 5 * 60;
    c->auth_state = GFN_AUTH_WAITING;
    snprintf(c->status, sizeof(c->status), "On %.40s, Steam asks for a code: type %s there", hosts[pick].name, pin);
    return true;
}

void steam_link_login_tick(GfnClient *c)
{
    if (!g.pairing) return;
    if (c->auth_state != GFN_AUTH_WAITING) {
        /* Cancelled from the screen. */
        steam_pair_cancel(&g.pair);
        g.pairing = false;
        return;
    }
    const SteamPairState state = steam_pair_poll(&g.pair);
    if (state == STEAM_PAIR_WAITING) return;
    g.pairing = false;
    if (state == STEAM_PAIR_DONE) {
        /* A PC paired again keeps its games; the one in use joins the others. */
        Recent kept[SL_RECENT_MAX];
        unsigned kept_count = 0;
        for (unsigned k = 0; k < g.other_count; ++k)
            if (g.others[k].host.client_id == g.pair.host.client_id) {
                kept_count = g.others[k].recent_count;
                memcpy(kept, g.others[k].recent, sizeof(kept));
                drop_other(k);
                break;
            }
        if (g.paired && g.host.client_id != g.pair.host.client_id) push_current();
        LightLock_Lock(&g_lock);
        if (!g.paired) {
            memcpy(g.recent, kept, sizeof(kept));
            g.recent_count = kept_count;
        }
        g.host = g.pair.host;
        if (g.pair.steamid) g.host.steamid = g.pair.steamid;
        memcpy(g.host_secret, g.pair.secret, 32);
        g.paired = true;
        LightLock_Unlock(&g_lock);
        steam_pair_cancel(&g.pair);
        save();
        publish_pcs();
        fill_tokens(c);
        c->auth_state = GFN_AUTH_LOGGED_IN;
        c->game_count = 0;
        gfn_fetch_library(c);
        snprintf(c->status, sizeof(c->status), "Paired with %.60s", g.host.name);
        diagnostic_log("STEAM", "paired (%u PCs)", g.other_count + 1);
        return;
    }
    steam_pair_cancel(&g.pair);
    if (g.paired) {
        /* Pairing another PC: the one paired before is still there. */
        fill_tokens(c);
        c->auth_state = GFN_AUTH_LOGGED_IN;
        snprintf(c->status, sizeof(c->status), "%s", state == STEAM_PAIR_WRONG_PIN
                 ? "The code typed on the PC didn't match; nothing changed."
                 : state == STEAM_PAIR_TIMEOUT ? "The pairing timed out; nothing changed."
                 : "The other PC didn't pair; nothing changed.");
        return;
    }
    c->auth_state = GFN_AUTH_ERROR;
    if (state == STEAM_PAIR_WRONG_PIN)
        snprintf(c->status, sizeof(c->status), "The code typed on the PC didn't match. Press X to get a new one.");
    else if (state == STEAM_PAIR_TIMEOUT)
        snprintf(c->status, sizeof(c->status), "The pairing timed out. Press X to try again.");
    else if (g.pair.result == 1 || g.pair.result == 8)
        snprintf(c->status, sizeof(c->status), "The pairing was declined on the PC. Press X to try again.");
    else
        snprintf(c->status, sizeof(c->status), "Steam refused the pairing (%s). Press X to try again.",
                 steam_pair_result_name(g.pair.result));
}

/* ---- The library ----------------------------------------------------------------- */

void steam_link_note_activity(int activity, uint64_t gameid, const char *name)
{
    /* 2: a game (the desktop and Big Picture are activities of their own). */
    if (activity != 2 || !gameid) return;
    LightLock_Lock(&g_lock);
    unsigned at = g.recent_count;
    for (unsigned i = 0; i < g.recent_count; ++i)
        if (g.recent[i].gameid == gameid) at = i;
    Recent entry = { gameid, "" };
    if (at < g.recent_count) entry = g.recent[at];
    else if (g.recent_count < SL_RECENT_MAX) ++g.recent_count;
    else at = SL_RECENT_MAX - 1;
    if (name && name[0]) snprintf(entry.name, sizeof(entry.name), "%s", name);
    memmove(&g.recent[1], &g.recent[0], at * sizeof(Recent));
    g.recent[0] = entry;
    g.recent_dirty = true;
    LightLock_Unlock(&g_lock);
}

/* A game Steam named only by its id: the store knows its name. */
static void fetch_name(Recent *r)
{
    char url[128];
    snprintf(url, sizeof(url), "https://store.steampowered.com/api/appdetails?appids=%u&filters=basic",
             (unsigned)r->gameid);
    HttpResponse response;
    if (!http_request("GET", url, "Kasumi-3DS", NULL, 0, NULL, 256 * 1024, &response)) return;
    json_error_t error;
    json_t *root = response.status == 200 && response.body ? json_loadb(response.body, response.size, 0, &error) : NULL;
    char key[16];
    snprintf(key, sizeof(key), "%u", (unsigned)r->gameid);
    json_t *data = json_object_get(json_object_get(root, key), "data");
    const char *name = json_string_value(json_object_get(data, "name"));
    if (name && name[0]) snprintf(r->name, sizeof(r->name), "%s", name);
    json_decref(root);
    http_response_free(&response);
}

static void add_game(GfnClient *c, const char *title, const char *id, uint64_t appid)
{
    if (c->game_count >= GFN_MAX_GAMES) return;
    GfnGame *game = &c->games[c->game_count++];
    memset(game, 0, sizeof(*game));
    snprintf(game->title, sizeof(game->title), "%s", title);
    snprintf(game->app_id, sizeof(game->app_id), "%s", id);
    snprintf(game->store, sizeof(game->store), "Steam");
    /* Only Steam's own games have store art (a shortcut's id is above 32 bits). */
    if (appid && appid < 0x100000000ull) {
        snprintf(game->image_url, sizeof(game->image_url),
                 "https://shared.steamstatic.com/store_item_assets/steam/apps/%u/library_600x900.jpg", (unsigned)appid);
        snprintf(game->wide_url, sizeof(game->wide_url),
                 "https://shared.steamstatic.com/store_item_assets/steam/apps/%u/header.jpg", (unsigned)appid);
    }
}

bool steam_link_fetch_library(GfnClient *c)
{
    load();
    if (!g.paired) {
        snprintf(c->status, sizeof(c->status), "Pair with your PC first (press X)");
        return false;
    }
    Recent recent[SL_RECENT_MAX];
    LightLock_Lock(&g_lock);
    const unsigned count = g.recent_count;
    memcpy(recent, g.recent, sizeof(recent));
    LightLock_Unlock(&g_lock);
    /* A few unnamed games per refresh: each is a request to the store. */
    unsigned named = 0;
    bool changed = false;
    for (unsigned i = 0; i < count && named < 4; ++i) {
        if (recent[i].name[0] || recent[i].gameid >= 0x100000000ull) continue;
        fetch_name(&recent[i]);
        changed |= recent[i].name[0] != '\0';
        ++named;
    }
    if (changed) {
        LightLock_Lock(&g_lock);
        for (unsigned i = 0; i < count && i < g.recent_count; ++i)
            if (g.recent[i].gameid == recent[i].gameid && !g.recent[i].name[0])
                snprintf(g.recent[i].name, sizeof(g.recent[i].name), "%s", recent[i].name);
        g.recent_dirty = true;
        LightLock_Unlock(&g_lock);
    }
    if (g.recent_dirty) save();
    c->game_count = 0;
    add_game(c, "Steam Big Picture", SL_BIG_PICTURE, 0);
    add_game(c, "PC desktop", SL_DESKTOP, 0);
    for (unsigned i = 0; i < count; ++i) {
        char id[24], title[96];
        snprintf(id, sizeof(id), "%llu", (unsigned long long)recent[i].gameid);
        if (recent[i].name[0]) snprintf(title, sizeof(title), "%s", recent[i].name);
        else snprintf(title, sizeof(title), "Steam game %llu", (unsigned long long)recent[i].gameid);
        add_game(c, title, id, recent[i].gameid);
    }
    c->catalog_total = c->game_count;
    snprintf(c->status, sizeof(c->status), "%s: Big Picture, the desktop and %u game%s played here", g.host.name,
             count, count == 1 ? "" : "s");
    return true;
}

static bool contains_nocase(const char *text, const char *needle)
{
    const size_t n = strlen(needle);
    for (; *text; ++text)
        if (!strncasecmp(text, needle, n)) return true;
    return n == 0;
}

bool steam_link_search(GfnClient *c, const char *query)
{
    if (!steam_link_fetch_library(c)) return false;
    size_t kept = 0;
    for (size_t i = 0; i < c->game_count; ++i) {
        if (!contains_nocase(c->games[i].title, query)) continue;
        if (kept != i) c->games[kept] = c->games[i];
        ++kept;
    }
    c->game_count = kept;
    snprintf(c->status, sizeof(c->status), "%lu match \"%.40s\"", (unsigned long)kept, query);
    return true;
}

/* ---- Streams --------------------------------------------------------------------- */

static void session_failed(GfnClient *c, const char *why, const char *code)
{
    c->session_state = GFN_SESSION_ERROR;
    snprintf(c->status, sizeof(c->status), "%s", why);
    snprintf(c->fail_code, sizeof(c->fail_code), "%s", code);
}

/* The paired PC, at its address now (it may have changed). */
static bool find_host(SteamHost *host)
{
    if (g.host.ip && steam_discover(&g.id, g.host.ip, host, 1, 1200) == 1 && host->client_id == g.host.client_id)
        return true;
    SteamHost hosts[8];
    const int found = steam_discover(&g.id, 0, hosts, 8, 2000);
    for (int i = 0; i < found; ++i) {
        if (hosts[i].client_id != g.host.client_id) continue;
        *host = hosts[i];
        if (host->ip != g.host.ip) {
            g.host.ip = host->ip;
            save();
        }
        return true;
    }
    return false;
}

bool steam_link_start_session(GfnClient *c, const GfnGame *game)
{
    c->session_state = GFN_SESSION_IDLE;
    c->fail_code[0] = c->end_error_code[0] = '\0';
    c->queue_position = 0;
    g_cancel = false;
    load();
    if (!g.paired || !game) {
        session_failed(c, "Pair with your PC first (Settings > Account).", "login");
        return false;
    }
    SteamHost host;
    if (!find_host(&host)) {
        char text[160];
        snprintf(text, sizeof(text), "%.40s isn't answering. Is it on, with Steam running, on the same Wi-Fi?",
                 g.host.name);
        session_failed(c, text, "offline");
        return false;
    }
    if (!host.steamid) host.steamid = g.host.steamid;
    SteamStreamRequest request;
    memset(&request, 0, sizeof(request));
    request.width = 800;
    request.height = 480;
    request.fps = stream_profile_fps();
    if (!strcmp(game->app_id, SL_DESKTOP)) {
        request.stream_interface = 3;
    } else {
        request.stream_interface = 2;
        if (strcmp(game->app_id, SL_BIG_PICTURE)) request.gameid = strtoull(game->app_id, NULL, 10);
    }
    snprintf(c->session_id, sizeof(c->session_id), "steam");
    c->session_state = GFN_SESSION_SETUP;
    snprintf(c->status, sizeof(c->status), "Asking %.40s to start streaming...", host.name);
    diagnostic_log("STEAM", "start %s (%s)", game->app_id, game->title);
    /* Launching a game or Big Picture can take a while on the PC. */
    const bool granted = !g_cancel && steam_request_stream(&g.id, g.host_secret, &host, &request, &g.grant, 20000,
                                                           &g_cancel);
    if (g_cancel) {
        /* A grant that came anyway is left to expire: we never connect. */
        memset(c->session_id, 0, sizeof(c->session_id));
        memset(&g.grant, 0, sizeof(g.grant));
        c->session_state = GFN_SESSION_IDLE;
        snprintf(c->status, sizeof(c->status), "Cancelled");
        return false;
    }
    if (!granted) {
        memset(c->session_id, 0, sizeof(c->session_id));
        session_failed(c, steam_stream_result_text(g.grant.result),
                       g.grant.result == 1 ? "unpaired" : g.grant.result < 0 ? "offline" : "refused");
        return false;
    }
    g.host.ip = host.ip;
    if (host.steamid) g.host.steamid = host.steamid;
    steam_ip_format(host.ip, c->media_ip, sizeof(c->media_ip));
    snprintf(c->server_ip, sizeof(c->server_ip), "%s", c->media_ip);
    c->media_port = g.grant.port;
    c->session_state = GFN_SESSION_READY;
    snprintf(c->status, sizeof(c->status), "Connecting to %.40s...", host.name);
    return true;
}

bool steam_link_recover_session(GfnClient *c, const GfnGame *game)
{
    /* The PC keeps the game running: just ask it to stream again. */
    return game && steam_link_start_session(c, game);
}

bool steam_link_stop_session(GfnClient *c)
{
    /* The stream already said goodbye; the game keeps running on the PC. */
    memset(c->session_id, 0, sizeof(c->session_id));
    memset(&g.grant, 0, sizeof(g.grant));
    c->session_state = GFN_SESSION_IDLE;
    snprintf(c->status, sizeof(c->status), "Stream ended; the PC keeps running");
    if (g.recent_dirty) {
        save();
        gfn_fetch_library(c);
    }
    return true;
}

bool steam_link_signal_start(NvstSignal *s, const GfnClient *client)
{
    nvst_signal_close(s);
    if (!g.grant.session_key_size || !client->media_port) {
        s->state = NVST_SIGNAL_ERROR;
        snprintf(s->status, sizeof(s->status), "The stream isn't ready");
        return false;
    }
    s->steam = true;
    s->steam_ip = steam_ip_parse(client->media_ip);
    s->steam_port = (uint16_t)client->media_port;
    memcpy(s->steam_key, g.grant.session_key, sizeof(s->steam_key));
    s->steam_key_size = (uint8_t)g.grant.session_key_size;
    s->steam_id = g.host.steamid;
    s->steam_fps = stream_profile_fps();
    /* Steam starts at the rate asked and adapts; the 3DS Wi-Fi does a few
     * Mbit/s at best. */
    s->steam_kbps = stream_profile_weak() ? 1500 : s->steam_fps >= 60 ? 4000 : 3000;
    /* The transport starts once an "offer" is there. */
    s->offer_sdp = strdup("steam");
    s->offer_size = 5;
    s->started_ms = osGetTime();
    s->state = NVST_SIGNAL_OFFER;
    snprintf(s->status, sizeof(s->status), "Connecting to your PC...");
    return s->offer_sdp != NULL;
}
