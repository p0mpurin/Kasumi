#include "xcloud.h"
#include "xcloud_net.h"
#include "xbox_console.h"
#include "platform.h"
#include "i18n.h"

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
#include "webrtc_transport.h"
#include "xcloud_stream.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

/* xbox.com's public client: its device code sign-in works for any device. */
#define XC_CLIENT_ID "1f907974-e22b-4810-a9de-d9647380c97e"
#define XC_SCOPE "xboxlive.signin%20openid%20profile%20offline_access"
#define XC_TOKEN_URL "https://login.microsoftonline.com/consumers/oauth2/v2.0/token"
/* Microsoft's gateways refuse requests without a browser user agent (403). */
#define XC_UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) " \
              "Chrome/130.0.0.0 Safari/537.36"
#define XC_LOGIN_PATH APP_DATA_DIR "/xcloud-login.json"
#define XC_LOGIN_TMP APP_DATA_DIR "/xcloud-login.tmp"
#define XC_LIBRARY_PATH APP_DATA_DIR "/xcloud-library.json"
#define XC_RESPONSE_MAX (2 * 1024 * 1024)
/* Entitled titles are fetched in pages; the catalog names them in batches. */
#define XC_TITLE_PAGES 12
#define XC_CATALOG_BATCH 40

static struct {
    bool loaded;
    bool has_cloud;     /* true when xCloud (xgpuweb / xgpuwebf2p) is active */
    char base[128];     /* xCloud region session service, no trailing slash */
    char region[32];
    char offering[16];  /* xgpuweb (Game Pass) or xgpuwebf2p (free to play) */
    char home_base[128]; /* xHome region session service, no trailing slash */
    char home_region[32];
    char home_token[8192];
    int64_t home_token_expires_at;
    char web_xsts[4096];
    char uhs[64];
    int64_t web_xsts_expires_at;
    bool home_session;  /* true while running a Remote Play (/home) session */
    bool home_checked;  /* streaming_token() already attempted xhome this run */
    bool connect_sent;  /* /connect for the current session */
    u64 next_state_at, next_keepalive_at;
    unsigned keepalive_failures;
    /* The session's last state (logged on change), when it started, and
     * when its console started (Provisioning/waking) for the start-up limit. */
    char last_state[32];
    u64 started_at, provisioning_at;
    /* The service's wait estimate while queued (0: none). */
    unsigned wait_seconds;
} g;

/* A console that never finishes starting: the session is given up. */
#define XC_PROVISION_LIMIT_MS 150000
#define XC_UNKNOWN_LIMIT_MS 180000

static PlatLock g_login_lock = PLAT_LOCK_INIT;
static char g_auth_header[8300];
static volatile bool g_selected;
static volatile bool g_home_session;
static volatile bool g_decode_720p;

void xcloud_select(bool xbox) { g_selected = xbox; }
bool xcloud_selected(void) { return g_selected; }
bool xcloud_decode_720p(void) { return g_decode_720p; }
bool xcloud_is_home_session(void) { return g_home_session; }
bool xcloud_available(void) { return true; }

static const char *session_base(void)
{
    return (g.home_session && g.home_base[0]) ? g.home_base : g.base;
}

static const char *session_token(const GfnClient *c)
{
    return (g.home_session && g.home_token[0]) ? g.home_token : c->access_token;
}

static void copy_string(char *out, size_t size, json_t *object, const char *key)
{
    const char *value = json_string_value(json_object_get(object, key));
    snprintf(out, size, "%s", value ? value : "");
}

static json_t *response_json(const HttpResponse *r)
{
    json_error_t error;
    return r->body ? json_loadb(r->body, r->size, 0, &error) : NULL;
}

/* ---- Saved login: tokens, region ------------------------------------------------ */

static json_int_t integer(json_t *object, const char *key)
{
    json_t *value = json_object_get(object, key);
    return json_is_integer(value) ? json_integer_value(value) : 0;
}

static json_t *login_file(void)
{
    json_error_t error;
    json_t *root = json_load_file(XC_LOGIN_PATH, 0, &error);
    if (json_is_object(root)) return root;
    json_decref(root);
    return NULL;
}

static void strip_trailing_slashes(char *url)
{
    size_t n = strlen(url);
    while (n > 8 && url[n - 1] == '/') url[--n] = '\0';
}

static void apply_login_json(json_t *root)
{
    if (!root) return;
    copy_string(g.base, sizeof(g.base), root, "base");
    copy_string(g.region, sizeof(g.region), root, "region");
    copy_string(g.offering, sizeof(g.offering), root, "offering");
    copy_string(g.home_base, sizeof(g.home_base), root, "home_base");
    copy_string(g.home_region, sizeof(g.home_region), root, "home_region");
    copy_string(g.home_token, sizeof(g.home_token), root, "home_token");
    g.home_token_expires_at = integer(root, "home_expires_at");
    if (!xcloud_valid_gs_base(g.base)) g.base[0] = '\0';
    if (!xcloud_valid_gs_base(g.home_base)) g.home_base[0] = '\0';
    strip_trailing_slashes(g.base);
    strip_trailing_slashes(g.home_base);
}

static void login_load(void)
{
    if (g.loaded) return;
    g.loaded = true;
    plat_lock(&g_login_lock);
    json_t *root = login_file();
    plat_unlock(&g_login_lock);
    apply_login_json(root);
    json_decref(root);
}

static bool login_save(const GfnClient *c)
{
    const char *cloud_tok = g.has_cloud ? c->access_token : "";
    const int64_t cloud_exp = g.has_cloud ? c->token_expires_at : 0;
    json_t *root = json_pack("{s:s,s:s,s:s,s:s,s:s,s:s,s:I,s:s,s:s,s:I,s:s,s:I}",
                             "base", g.base, "region", g.region, "offering", g.offering,
                             "home_base", g.home_base, "home_region", g.home_region,
                             "home_token", g.home_token, "home_expires_at", (json_int_t)g.home_token_expires_at,
                             "refresh_token", c->refresh_token,
                             "msa_token", c->id_token, "msa_expires_at", (json_int_t)c->client_token_expires_at,
                             "stream_token", cloud_tok, "stream_expires_at", (json_int_t)cloud_exp);
    if (!root) return false;
    plat_lock(&g_login_lock);
    bool ok = json_dump_file(root, XC_LOGIN_TMP, JSON_COMPACT) == 0;
    if (ok) {
        remove(XC_LOGIN_PATH);
        ok = rename(XC_LOGIN_TMP, XC_LOGIN_PATH) == 0;
    }
    if (!ok) remove(XC_LOGIN_TMP);
    plat_unlock(&g_login_lock);
    json_decref(root);
    return ok;
}

bool xcloud_load_login(GfnClient *c)
{
    g.loaded = true;
    plat_lock(&g_login_lock);
    json_t *root = login_file();
    plat_unlock(&g_login_lock);
    if (!root) return false;
    apply_login_json(root);
    copy_string(c->refresh_token, sizeof(c->refresh_token), root, "refresh_token");
    copy_string(c->id_token, sizeof(c->id_token), root, "msa_token");
    copy_string(c->access_token, sizeof(c->access_token), root, "stream_token");
    c->client_token_expires_at = integer(root, "msa_expires_at");
    c->token_expires_at = integer(root, "stream_expires_at");
    json_decref(root);
    provider_xbox(&c->login_provider);
    g.has_cloud = g.base[0] && c->access_token[0];
    /* For xHome-only accounts (no Game Pass / xCloud), keep c->access_token
     * populated with the xHome token so gfn_has_session() and keep_login_tick()
     * recognise the signed-in session without breaking cloud detection. */
    if (!g.has_cloud && g.home_token[0]) {
        snprintf(c->access_token, sizeof(c->access_token), "%s", g.home_token);
        c->token_expires_at = g.home_token_expires_at;
    } else if (!c->access_token[0] && c->refresh_token[0]) {
        snprintf(c->access_token, sizeof(c->access_token), "renew");
    }
    return c->refresh_token[0] && (g.base[0] || g.home_base[0]);
}

bool xcloud_login_saved(void)
{
    plat_lock(&g_login_lock);
    json_t *root = login_file();
    plat_unlock(&g_login_lock);
    const char *rt = root ? json_string_value(json_object_get(root, "refresh_token")) : NULL;
    const bool saved = rt && rt[0];
    json_decref(root);
    return saved;
}

void xcloud_sign_out(void)
{
    plat_lock(&g_login_lock);
    remove(XC_LOGIN_PATH);
    remove(XC_LOGIN_TMP);
    remove(XC_LIBRARY_PATH);
    plat_unlock(&g_login_lock);
    xbox_consoles_clear();
    memset(&g, 0, sizeof(g));
    g_home_session = false;
    g_decode_720p = false;
}

/* ---- Microsoft account ---------------------------------------------------------- */

static bool token_form(GfnClient *c, const char *form, char *error_out, size_t error_size, long *status)
{
    static const char *const headers[] = {
        "Accept: application/json", "Content-Type: application/x-www-form-urlencoded"
    };
    HttpResponse r;
    if (error_out && error_size) error_out[0] = '\0';
    *status = 0;
    if (!http_request("POST", XC_TOKEN_URL, XC_UA, headers, ARRAY_SIZE(headers), form, 256 * 1024, &r)) {
        snprintf(c->status, sizeof(c->status), i18n_text("Can't reach Microsoft's sign-in (%.80s)"), r.error);
        return false;
    }
    *status = r.status;
    json_t *root = response_json(&r);
    bool ok = false;
    if (r.status == 200 && root && json_is_string(json_object_get(root, "access_token"))) {
        copy_string(c->id_token, sizeof(c->id_token), root, "access_token");
        const char *refresh = json_string_value(json_object_get(root, "refresh_token"));
        if (refresh && refresh[0]) snprintf(c->refresh_token, sizeof(c->refresh_token), "%s", refresh);
        json_t *expires = json_object_get(root, "expires_in");
        c->client_token_expires_at = (int64_t)time(NULL) +
            (json_is_integer(expires) ? json_integer_value(expires) : 3600);
        ok = true;
    } else if (root && error_out && error_size) {
        copy_string(error_out, error_size, root, "error");
    }
    json_decref(root);
    http_response_free(&r);
    return ok;
}

/* A fresh Microsoft access token (they last an hour). */
static bool msa_access(GfnClient *c)
{
    if (c->id_token[0] && (int64_t)time(NULL) + 300 < c->client_token_expires_at) return true;
    if (!c->refresh_token[0]) return false;
    char *encoded = http_url_encode(c->refresh_token);
    if (!encoded) return false;
    const size_t length = strlen(encoded) + 256;
    char *form = malloc(length);
    if (!form) {
        free(encoded);
        return false;
    }
    snprintf(form, length, "client_id=%s&grant_type=refresh_token&scope=%s&refresh_token=%s",
             XC_CLIENT_ID, XC_SCOPE, encoded);
    free(encoded);
    char code[48] = "";
    long status = 0;
    const bool ok = token_form(c, form, code, sizeof(code), &status);
    free(form);
    diagnostic_log("XCLOUD", "microsoft refresh http=%ld %s%s", status, ok ? "ok" : "failed ", ok ? "" : code);
    if (!ok && (status == 400 || status == 401)) {
        c->auth_state = GFN_AUTH_ERROR;
        snprintf(c->status, sizeof(c->status), i18n_text("Your Microsoft sign-in has expired. Press X to sign in again."));
    }
    return ok;
}

static const char *const g_xbl_headers[] = {
    "x-xbl-contract-version: 1", "Accept: application/json", "Content-Type: application/json",
    "Origin: https://www.xbox.com", "Referer: https://www.xbox.com/"
};

static bool xbl_user_token(GfnClient *c, char *out, size_t size)
{
    json_t *props = json_pack("{s:s,s:s,s:s}", "AuthMethod", "RPS", "SiteName", "user.auth.xboxlive.com",
                              "RpsTicket", "");
    char *ticket = malloc(strlen(c->id_token) + 4);
    if (!props || !ticket) {
        json_decref(props);
        free(ticket);
        return false;
    }
    sprintf(ticket, "d=%s", c->id_token);
    json_object_set_new(props, "RpsTicket", json_string(ticket));
    free(ticket);
    json_t *req = json_pack("{s:o,s:s,s:s}", "Properties", props, "RelyingParty", "http://auth.xboxlive.com",
                            "TokenType", "JWT");
    char *body = req ? json_dumps(req, JSON_COMPACT) : NULL;
    json_decref(req);
    if (!body) return false;
    HttpResponse r;
    const bool sent = http_request("POST", "https://user.auth.xboxlive.com/user/authenticate", XC_UA,
                                   g_xbl_headers, ARRAY_SIZE(g_xbl_headers), body, 256 * 1024, &r);
    free(body);
    if (!sent) {
        snprintf(c->status, sizeof(c->status), i18n_text("Can't reach Xbox Live (%.80s)"), r.error);
        return false;
    }
    json_t *root = response_json(&r);
    const char *user = json_string_value(json_object_get(root, "Token"));
    diagnostic_log("XCLOUD", "user token http=%ld %s", r.status, user ? "ok" : "missing");
    const bool ok = r.status == 200 && user && strlen(user) < size;
    if (ok) snprintf(out, size, "%s", user);
    else snprintf(c->status, sizeof(c->status), i18n_text("Xbox Live refused the sign-in (HTTP %ld)"), r.status);
    json_decref(root);
    http_response_free(&r);
    return ok;
}

static bool xsts_authorize(GfnClient *c, const char *user_token, const char *relying_party,
                           char *token_out, size_t token_size, char *uhs_out, size_t uhs_size)
{
    json_t *request = json_pack("{s:{s:s,s:[s]},s:s,s:s}", "Properties", "SandboxId", "RETAIL", "UserTokens",
                                user_token, "RelyingParty", relying_party, "TokenType", "JWT");
    char *text = request ? json_dumps(request, JSON_COMPACT) : NULL;
    json_decref(request);
    if (!text) return false;
    HttpResponse r;
    const bool sent = http_request("POST", "https://xsts.auth.xboxlive.com/xsts/authorize", XC_UA,
                                   g_xbl_headers, ARRAY_SIZE(g_xbl_headers), text, 256 * 1024, &r);
    free(text);
    if (!sent) {
        snprintf(c->status, sizeof(c->status), i18n_text("Can't reach Xbox Live (%.80s)"), r.error);
        return false;
    }
    json_t *root = response_json(&r);
    const char *xsts = json_string_value(json_object_get(root, "Token"));
    /* XErr 2148916233: no Xbox profile yet; 2148916238: a child account. */
    const json_int_t xerr = json_integer_value(json_object_get(root, "XErr"));
    diagnostic_log("XCLOUD", "xsts (%s) http=%ld %s xerr=%lld", relying_party, r.status,
                   xsts ? "ok" : "missing", (long long)xerr);
    bool ok = r.status == 200 && xsts && strlen(xsts) < token_size;
    if (ok) {
        snprintf(token_out, token_size, "%s", xsts);
        if (uhs_out && uhs_size) {
            json_t *claims = json_object_get(root, "DisplayClaims");
            json_t *xui = json_object_get(claims, "xui");
            json_t *first = json_is_array(xui) ? json_array_get(xui, 0) : NULL;
            const char *uhs = json_string_value(json_object_get(first, "uhs"));
            if (uhs && strlen(uhs) < uhs_size) snprintf(uhs_out, uhs_size, "%s", uhs);
            else ok = false;
        }
    } else if (xerr == 2148916233LL) {
        snprintf(c->status, sizeof(c->status), i18n_text("This Microsoft account has no Xbox profile yet. Sign in once on "
                 "xbox.com to make one, then try again."));
    } else if (xerr == 2148916238LL) {
        snprintf(c->status, sizeof(c->status), i18n_text("A child account needs a parent to allow cloud gaming first."));
    } else {
        snprintf(c->status, sizeof(c->status), i18n_text("Xbox Live refused the sign-in (HTTP %ld)"), r.status);
    }
    json_decref(root);
    http_response_free(&r);
    return ok;
}

static bool ensure_web_xsts(GfnClient *c)
{
    const int64_t now = (int64_t)time(NULL);
    if (g.web_xsts[0] && g.uhs[0] && now + 300 < g.web_xsts_expires_at) return true;
    if (!msa_access(c)) return false;
    login_save(c);
    static char user_tok[4096];
    if (!xbl_user_token(c, user_tok, sizeof(user_tok))) return false;
    if (!xsts_authorize(c, user_tok, "http://xboxlive.com", g.web_xsts, sizeof(g.web_xsts), g.uhs, sizeof(g.uhs)))
        return false;
    g.web_xsts_expires_at = now + 3600;
    return true;
}

static bool login_offering(GfnClient *c, const char *xsts, const char *offering, char *token_out, size_t token_size,
                           int64_t *expires_out, char *base_out, size_t base_size, char *region_out,
                           size_t region_size)
{
    json_t *request = json_pack("{s:s,s:s}", "token", xsts, "offeringId", offering);
    char *text = request ? json_dumps(request, JSON_COMPACT) : NULL;
    json_decref(request);
    if (!text) return false;
    char url[128];
    snprintf(url, sizeof(url), "https://%s.gssv-play-prod.xboxlive.com/v2/login/user", offering);
    static const char *const headers[] = {
        "Content-Type: application/json", "Accept: application/json", "x-gssv-client: XboxComBrowser",
        "Cache-Control: no-store, must-revalidate, no-cache"
    };
    HttpResponse r;
    const bool sent = http_request("POST", url, XC_UA, headers, ARRAY_SIZE(headers), text, 512 * 1024, &r);
    free(text);
    if (!sent) {
        diagnostic_log("XCLOUD", "streaming login %s net error: %.80s", offering, r.error);
        snprintf(c->status, sizeof(c->status), i18n_text("Can't reach Xbox Cloud Gaming (%.80s)"), r.error);
        return false;
    }
    json_t *root = response_json(&r);
    const char *token = json_string_value(json_object_get(root, "gsToken"));
    diagnostic_log("XCLOUD", "streaming login %s http=%ld %s", offering, r.status, token ? "ok" : "refused");
    if (r.status == 200 && token && strlen(token) < token_size) {
        snprintf(token_out, token_size, "%s", token);
        json_t *duration = json_object_get(root, "durationInSeconds");
        *expires_out = (int64_t)time(NULL) + (json_is_integer(duration) ? json_integer_value(duration) : 3600);
        json_t *regions = json_object_get(json_object_get(root, "offeringSettings"), "regions");
        size_t index;
        json_t *region;
        char first[128] = "", first_name[32] = "";
        bool kept = false;
        json_array_foreach(regions, index, region) {
            const char *uri = json_string_value(json_object_get(region, "baseUri"));
            const char *name = json_string_value(json_object_get(region, "name"));
            if (!uri || !name) continue;
            if (!xcloud_valid_gs_base(uri) || strlen(uri) >= base_size) {
                diagnostic_log("XCLOUD", "ignored %s baseUri: %.80s", offering, uri);
                continue;
            }
            if (!first[0] || json_is_true(json_object_get(region, "isDefault"))) {
                snprintf(first, sizeof(first), "%s", uri);
                snprintf(first_name, sizeof(first_name), "%s", name);
            }
            if (region_out[0] && !strcmp(region_out, name)) {
                snprintf(base_out, base_size, "%s", uri);
                kept = true;
            }
        }
        if (!kept && first[0]) {
            snprintf(base_out, base_size, "%s", first);
            snprintf(region_out, region_size, "%.*s", (int)(region_size - 1), first_name);
        }
        if (!base_out[0]) {
            snprintf(base_out, base_size, "https://%s.gssv-play-prod.xboxlive.com", offering);
            diagnostic_log("XCLOUD", "%s had no accepted region baseUri; falling back to %s", offering, base_out);
        }
        strip_trailing_slashes(base_out);
        diagnostic_log("XCLOUD", "%s region %s (%s), %lu offered, token lasts %llds", offering,
                       region_out[0] ? region_out : "-", base_out,
                       (unsigned long)json_array_size(regions),
                       (long long)(*expires_out - (int64_t)time(NULL)));
        json_decref(root);
        http_response_free(&r);
        return base_out[0] != '\0';
    }
    json_decref(root);
    http_response_free(&r);
    return false;
}

/* Streaming tokens for Xbox Remote Play (xhome) and xCloud (xgpuweb /
 * xgpuwebf2p). Remote Play works even without a Game Pass membership. */
static bool streaming_token(GfnClient *c)
{
    login_load();
    if (!msa_access(c)) return false;
    static char user_tok[4096], xsts[4096];
    if (!xbl_user_token(c, user_tok, sizeof(user_tok))) return false;
    if (!xsts_authorize(c, user_tok, "http://gssv.xboxlive.com/", xsts, sizeof(xsts), NULL, 0)) return false;

    /* Tokens still good before this renewal: a failed request (Wi-Fi, a
     * service hiccup) keeps them while they last instead of signing out. */
    const int64_t now = (int64_t)time(NULL);
    const bool had_home = g.home_token[0] && g.home_base[0] && now + 60 < g.home_token_expires_at;
    const bool had_cloud = g.has_cloud && g.base[0] && c->access_token[0] && now + 60 < c->token_expires_at;
    const bool home_ok = login_offering(c, xsts, "xhome", g.home_token, sizeof(g.home_token),
                                        &g.home_token_expires_at, g.home_base, sizeof(g.home_base),
                                        g.home_region, sizeof(g.home_region)) || had_home;
    g.home_checked = true;
    if (!home_ok) {
        g.home_token[0] = g.home_base[0] = g.home_region[0] = '\0';
        g.home_token_expires_at = 0;
    }

    static const char *const offerings[] = { "xgpuweb", "xgpuwebf2p" };
    bool cloud_ok = false;
    for (size_t i = 0; i < ARRAY_SIZE(offerings); ++i) {
        if (login_offering(c, xsts, offerings[i], c->access_token, sizeof(c->access_token), &c->token_expires_at,
                           g.base, sizeof(g.base), g.region, sizeof(g.region))) {
            snprintf(g.offering, sizeof(g.offering), "%s", offerings[i]);
            cloud_ok = true;
            break;
        }
    }
    cloud_ok = cloud_ok || had_cloud;
    g.has_cloud = cloud_ok;
    if (!cloud_ok) {
        g.base[0] = g.region[0] = g.offering[0] = '\0';
        if (home_ok) {
            snprintf(c->access_token, sizeof(c->access_token), "%s", g.home_token);
            c->token_expires_at = g.home_token_expires_at;
        } else {
            c->access_token[0] = '\0';
            c->token_expires_at = 0;
        }
    }
    if (home_ok || cloud_ok) {
        login_save(c);
        return true;
    }
    snprintf(c->status, sizeof(c->status), i18n_text("Xbox Cloud Gaming isn't available for this account or country."));
    return false;
}

bool xcloud_begin_login(GfnClient *c)
{
    static const char *const headers[] = {
        "Accept: application/json", "Content-Type: application/x-www-form-urlencoded"
    };
    /* Reset in-memory state for the new sign-in without wiping the saved
     * file until the new sign-in actually succeeds (or user signs out). */
    memset(&g, 0, sizeof(g));
    g.loaded = true;
    xbox_consoles_clear();
    provider_xbox(&c->login_provider);
    snprintf(c->status, sizeof(c->status), i18n_text("Contacting Microsoft sign-in..."));
    HttpResponse r;
    if (!http_request("POST", "https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode", XC_UA,
                      headers, ARRAY_SIZE(headers), "client_id=" XC_CLIENT_ID "&scope=" XC_SCOPE, 64 * 1024, &r)) {
        char date[16];
        if (http_clock_wrong(date, sizeof(date)))
            snprintf(c->status, sizeof(c->status), i18n_text("Your 3DS thinks it is %s, so the secure sign-in fails. Set the "
                     "date and time in System Settings, then try again."), date);
        else
            snprintf(c->status, sizeof(c->status), i18n_text("Couldn't reach Microsoft's sign-in (%.60s). Check Wi-Fi."), r.error);
        c->auth_state = GFN_AUTH_ERROR;
        return false;
    }
    json_t *root = response_json(&r);
    if (r.status != 200 || !root) {
        snprintf(c->status, sizeof(c->status), i18n_text("Microsoft sign-in: HTTP %ld"), r.status);
        json_decref(root);
        http_response_free(&r);
        c->auth_state = GFN_AUTH_ERROR;
        return false;
    }
    copy_string(c->device_code, sizeof(c->device_code), root, "device_code");
    copy_string(c->user_code, sizeof(c->user_code), root, "user_code");
    copy_string(c->verification_uri, sizeof(c->verification_uri), root, "verification_uri");
    json_t *interval = json_object_get(root, "interval");
    json_t *expires = json_object_get(root, "expires_in");
    c->poll_interval = json_is_integer(interval) ? (int)json_integer_value(interval) : 5;
    if (c->poll_interval < 1) c->poll_interval = 5;
    const int64_t now = (int64_t)time(NULL);
    c->next_poll_at = now + c->poll_interval;
    c->challenge_expires_at = now + (json_is_integer(expires) ? json_integer_value(expires) : 900);
    json_decref(root);
    http_response_free(&r);
    if (!c->device_code[0] || !c->user_code[0] || !c->verification_uri[0]) {
        snprintf(c->status, sizeof(c->status), i18n_text("Microsoft sign-in response incomplete"));
        c->auth_state = GFN_AUTH_ERROR;
        return false;
    }
    c->auth_state = GFN_AUTH_WAITING;
    snprintf(c->status, sizeof(c->status), i18n_text("Signing in to Xbox: open the link on a phone or PC and enter the code"));
    diagnostic_log("XCLOUD", "device code issued, lasts %llds", (long long)(c->challenge_expires_at - now));
    return true;
}

void xcloud_login_tick(GfnClient *c)
{
    if (c->auth_state != GFN_AUTH_WAITING) return;
    const int64_t now = (int64_t)time(NULL);
    if (now >= c->challenge_expires_at) {
        c->auth_state = GFN_AUTH_ERROR;
        snprintf(c->status, sizeof(c->status), i18n_text("Sign-in code expired; press X to get a new one"));
        return;
    }
    if (now < c->next_poll_at) return;
    c->next_poll_at = now + c->poll_interval;
    char *encoded = http_url_encode(c->device_code);
    if (!encoded) return;
    const size_t length = strlen(encoded) + 192;
    char *form = malloc(length);
    if (!form) {
        free(encoded);
        return;
    }
    snprintf(form, length, "grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code&client_id=%s"
             "&device_code=%s", XC_CLIENT_ID, encoded);
    free(encoded);
    char code[48] = "";
    long status = 0;
    const bool ok = token_form(c, form, code, sizeof(code), &status);
    free(form);
    if (!ok) {
        if (!strcmp(code, "authorization_pending")) {
            snprintf(c->status, sizeof(c->status), i18n_text("Waiting for the Microsoft sign-in..."));
        } else if (!strcmp(code, "slow_down")) {
            c->poll_interval += 5;
        } else if (status) {
            diagnostic_log("XCLOUD", "sign-in poll http=%ld error=%s", status, code);
            c->auth_state = GFN_AUTH_ERROR;
            snprintf(c->status, sizeof(c->status), !strcmp(code, "authorization_declined")
                     ? i18n_text("The sign-in was declined. Press X to try again.") : i18n_text("Microsoft sign-in failed (%.40s)"), code);
        }
        return;
    }
    diagnostic_log("XCLOUD", "microsoft sign-in done");
    if (!streaming_token(c)) {
        c->auth_state = GFN_AUTH_ERROR;
        return;
    }
    c->auth_state = GFN_AUTH_LOGGED_IN;
    c->user_id[0] = '\0';
    const bool saved = login_save(c);
    snprintf(c->status, sizeof(c->status), i18n_text("Signed in to Xbox; %s"), saved ? i18n_text("saved to SD") : i18n_text("save failed"));
}

bool xcloud_keep_login(GfnClient *c)
{
    login_load();
    const int64_t now = (int64_t)time(NULL);
    const bool cloud_ok = g.has_cloud && c->access_token[0] && g.base[0] && now + 600 < c->token_expires_at;
    const bool home_ok = g.home_token[0] && g.home_base[0] && now + 600 < g.home_token_expires_at;
    if (g.home_session ? home_ok : (home_ok && (cloud_ok || !g.has_cloud))) return true;
    if (!g.home_session && cloud_ok && g.home_checked) return true;
    if (!streaming_token(c)) {
        /* A passing failure keeps the token while it lasts. */
        if (g.home_session) return g.home_token[0] && g.home_base[0] && now + 60 < g.home_token_expires_at;
        return (g.has_cloud && c->access_token[0] && g.base[0] && now + 60 < c->token_expires_at) ||
               (g.home_token[0] && g.home_base[0] && now + 60 < g.home_token_expires_at);
    }
    return true;
}

/* ---- Requests to the region's session service --------------------------------- */

static bool gs_request_raw(const char *base, const char *token, const char *method, const char *path,
                           const char *body, HttpResponse *r, char *err_out, size_t err_size)
{
    if (!base || !base[0] || !token || !token[0]) {
        if (err_out && err_size) snprintf(err_out, err_size, i18n_text("Xbox sign-in required."));
        return false;
    }
    char url[1024];
    const int n = snprintf(url, sizeof(url), "%s%s", base, path);
    if (n < 0 || (size_t)n >= sizeof(url)) {
        if (err_out && err_size) snprintf(err_out, err_size, i18n_text("Xbox request URL too long."));
        return false;
    }
    snprintf(g_auth_header, sizeof(g_auth_header), "Authorization: Bearer %s", token);
    /* The device info picks the stream: Microsoft fits the picture to the
     * client's screen. Xbox Cloud Gaming keeps the values tuned against the
     * web player (android gets the smallest picture, 720p); Remote Play uses
     * the ones its contributor tested with a console. */
    char info[900];
    if (g.home_session)
        snprintf(info, sizeof(info),
                 "X-MS-Device-Info: {\"appInfo\":{\"env\":{\"clientAppId\":\"www.xbox.com\",\"clientAppType\":\"browser\","
                 "\"clientAppVersion\":\"21.1.98\",\"clientSdkVersion\":\"8.5.3\",\"httpEnvironment\":\"prod\","
                 "\"sdkInstallId\":\"\"}},\"dev\":{\"hw\":{\"make\":\"Microsoft\",\"model\":\"unknown\",\"sdktype\":\"web\"},"
                 "\"os\":{\"name\":\"android\",\"ver\":\"22631.2715\",\"platform\":\"desktop\"},\"displayInfo\":{\"dimensions\":"
                 "{\"widthInPixels\":%u,\"heightInPixels\":%u},\"pixelDensity\":{\"dpiX\":1,\"dpiY\":1}}}}",
                 XCLOUD_STREAM_WIDTH, XCLOUD_STREAM_HEIGHT);
    else
        snprintf(info, sizeof(info),
                 "X-MS-Device-Info: {\"appInfo\":{\"env\":{\"clientAppId\":\"www.xbox.com\",\"clientAppType\":\"browser\","
                 "\"clientAppVersion\":\"26.1.97\",\"clientSdkVersion\":\"10.3.7\",\"httpEnvironment\":\"prod\","
                 "\"sdkInstallId\":\"\"}},\"dev\":{\"hw\":{\"make\":\"Nintendo\",\"model\":\"New 3DS\",\"sdktype\":\"web\"},"
                 "\"os\":{\"name\":\"android\",\"ver\":\"11.17\",\"platform\":\"mobile\"},\"displayInfo\":{\"dimensions\":"
                 "{\"widthInPixels\":%u,\"heightInPixels\":%u},\"pixelDensity\":{\"dpiX\":1,\"dpiY\":1}},"
                 "\"browser\":{\"browserName\":\"chrome\",\"browserVersion\":\"130.0\"}}}",
                 XCLOUD_STREAM_WIDTH, XCLOUD_STREAM_HEIGHT);
    const char *headers[] = {
        g_auth_header, "x-gssv-client: XboxComBrowser", info, "Accept: application/json",
        "Content-Type: application/json"
    };
    const bool sent = http_request(method, url, XC_UA, headers, ARRAY_SIZE(headers), body, XC_RESPONSE_MAX, r);
    if (!sent && err_out && err_size)
        snprintf(err_out, err_size, i18n_text("Xbox unreachable (%.80s)"), r->error);
    return sent;
}

static bool gs_request(GfnClient *c, const char *method, const char *path, const char *body, HttpResponse *r)
{
    return gs_request_raw(session_base(), session_token(c), method, path, body, r, c->status, sizeof(c->status));
}

/* Microsoft's error, if the body has one ("errorCode"/"code" and "message"). */
static void error_text(const HttpResponse *r, char *out, size_t size)
{
    out[0] = '\0';
    json_t *root = response_json(r);
    if (!root) return;
    json_t *details = json_object_get(root, "errorDetails");
    json_t *where = json_is_object(details) ? details : root;
    const char *code = json_string_value(json_object_get(where, "code"));
    if (!code) code = json_string_value(json_object_get(where, "errorCode"));
    const char *message = json_string_value(json_object_get(where, "message"));
    snprintf(out, size, "%s%s%s", code ? code : "", code && message ? ": " : "", message ? message : "");
    json_decref(root);
}

/* ---- Library & local consoles -------------------------------------------------- */

typedef struct {
    char title_id[48];
    char product_id[48];
} XcTitle;

static int compare_games(const void *a, const void *b)
{
    return strcasecmp(((const GfnGame *)a)->title, ((const GfnGame *)b)->title);
}

static void fill_names(GfnClient *c, XcTitle *titles, size_t count)
{
    static const char *const headers[] = {
        "Content-Type: application/json", "Accept: application/json", "ms-cv: 0",
        "calling-app-name: Xbox Cloud Gaming Web", "calling-app-version: 26.1.97"
    };
    for (size_t start = 0; start < count; start += XC_CATALOG_BATCH) {
        const size_t end = start + XC_CATALOG_BATCH < count ? start + XC_CATALOG_BATCH : count;
        json_t *ids = json_array();
        for (size_t i = start; i < end; ++i)
            if (titles[i].product_id[0]) json_array_append_new(ids, json_string(titles[i].product_id));
        json_t *request = json_pack("{s:o}", "Products", ids);
        char *text = request ? json_dumps(request, JSON_COMPACT) : NULL;
        json_decref(request);
        if (!text) continue;
        HttpResponse r;
        const bool sent = http_request("POST", "https://catalog.gamepass.com/v3/products?market=US&language=en-US"
                                       "&hydration=RemoteLowJade0", XC_UA, headers, ARRAY_SIZE(headers), text,
                                       XC_RESPONSE_MAX, &r);
        free(text);
        if (!sent) continue;
        json_t *root = r.status == 200 ? response_json(&r) : NULL;
        json_t *products = json_object_get(root, "Products");
        for (size_t i = start; i < end && json_is_object(products); ++i) {
            json_t *p = json_object_get(products, titles[i].product_id);
            if (!json_is_object(p)) continue;
            for (size_t k = 0; k < c->game_count; ++k) {
                GfnGame *game = &c->games[k];
                if (xcloud_is_home_store(game->store) || strcmp(game->app_id, titles[i].title_id)) continue;
                const char *name = json_string_value(json_object_get(p, "ProductTitle"));
                if (name && name[0]) snprintf(game->title, sizeof(game->title), "%s", name);
                const char *poster = json_string_value(json_object_get(json_object_get(p, "Image_Poster"), "URL"));
                const char *tile = json_string_value(json_object_get(json_object_get(p, "Image_Tile"), "URL"));
                const char *art = poster ? poster : tile;
                if (art && !strncmp(art, "//", 2) && strlen(art) + 6 < sizeof(game->image_url))
                    snprintf(game->image_url, sizeof(game->image_url), "https:%s", art);
                if (tile && !strncmp(tile, "//", 2) && strlen(tile) + 6 < sizeof(game->wide_url))
                    snprintf(game->wide_url, sizeof(game->wide_url), "https:%s", tile);
            }
        }
        json_decref(root);
        http_response_free(&r);
    }
}

static size_t append_console_entry(GfnClient *c, XboxConsole *list, unsigned *console_total,
                                   const char *id, const char *name, const char *type, const char *power,
                                   bool streaming, bool remote_mgmt, bool wireless, bool out_of_home)
{
    if (!id || !id[0] || strlen(id) >= sizeof(c->games[0].app_id)) return 0;
    const char *safe_type = (type && type[0]) ? type : "Xbox";
    const char *label = (name && name[0]) ? name : safe_type;
    diagnostic_log("XHOME", "console id=%s name=%s type=%s power=%s streaming=%d mgmt=%d",
                   id, label, safe_type, power ? power : "-", streaming ? 1 : 0, remote_mgmt ? 1 : 0);
    if (*console_total < XBOX_CONSOLE_MAX) {
        XboxConsole *xc = &list[(*console_total)++];
        memset(xc, 0, sizeof(*xc));
        snprintf(xc->id, sizeof(xc->id), "%s", id);
        snprintf(xc->name, sizeof(xc->name), "%s", label);
        snprintf(xc->type, sizeof(xc->type), "%s", safe_type);
        xc->power = xcloud_console_power(power);
        xc->streaming = streaming;
        xc->remote_mgmt = remote_mgmt;
        xc->wireless_warn = wireless;
        xc->out_of_home_warn = out_of_home;
    }
    if (!streaming || c->game_count >= GFN_MAX_GAMES) return 0;
    GfnGame *game = &c->games[c->game_count++];
    memset(game, 0, sizeof(*game));
    if (strncasecmp(label, "Xbox", 4) != 0)
        snprintf(game->title, sizeof(game->title), "Xbox: %s", label);
    else
        snprintf(game->title, sizeof(game->title), "%s", label);
    snprintf(game->app_id, sizeof(game->app_id), "%s", id);
    snprintf(game->store, sizeof(game->store), "Remote Play");
    return 1;
}

static size_t fetch_home_consoles(GfnClient *c, bool *had_error)
{
    if (had_error) *had_error = false;
    XboxConsole list[XBOX_CONSOLE_MAX];
    unsigned console_total = 0;
    size_t added = 0;
    bool primary_ok = false;

    /* Primary: native xHome GSSV endpoint using the xHome gsToken directly. */
    if (g.home_base[0] && g.home_token[0]) {
        HttpResponse r;
        char err[160] = "";
        if (gs_request_raw(g.home_base, g.home_token, "GET", "/v6/servers/home?mr=50", NULL, &r, err, sizeof(err))) {
            json_t *root = r.status == 200 ? response_json(&r) : NULL;
            diagnostic_log("XHOME", "v6/servers/home http=%ld bytes=%lu", r.status, (unsigned long)r.size);
            if (r.status == 200 && root) {
                primary_ok = true;
                json_t *results = json_object_get(root, "results");
                json_t *item;
                size_t index;
                json_array_foreach(results, index, item) {
                    const char *id = json_string_value(json_object_get(item, "serverId"));
                    const char *name = json_string_value(json_object_get(item, "deviceName"));
                    const char *type = json_string_value(json_object_get(item, "consoleType"));
                    const char *power = json_string_value(json_object_get(item, "powerState"));
                    json_t *st = json_object_get(item, "consoleStreamingEnabled");
                    const bool streaming = !json_is_false(st);
                    const bool wireless = json_is_true(json_object_get(item, "wirelessWarning"));
                    const bool out_of_home = json_is_true(json_object_get(item, "outOfHomeWarning"));
                    added += append_console_entry(c, list, &console_total, id, name, type, power,
                                                  streaming, true, wireless, out_of_home);
                }
            } else {
                char why[120];
                error_text(&r, why, sizeof(why));
                diagnostic_log("XHOME", "v6/servers/home failed http=%ld %.80s", r.status, why);
                snprintf(c->status, sizeof(c->status), i18n_text("Xbox Remote Play list: HTTP %ld"), r.status);
            }
            json_decref(root);
            http_response_free(&r);
        } else {
            diagnostic_log("XHOME", "v6/servers/home net error: %.80s", err);
            snprintf(c->status, sizeof(c->status), "%s", err);
        }
    }

    if (console_total > 0) {
        xbox_consoles_set(list, console_total);
        return added;
    }

    /* Fallback: XCCS device list (also surfaces consoles with Remote Play disabled). */
    if (!ensure_web_xsts(c)) {
        if (had_error && !primary_ok) *had_error = true;
        if (primary_ok) xbox_consoles_set(list, 0);
        return 0;
    }
    static char xbl_auth[4300];
    snprintf(xbl_auth, sizeof(xbl_auth), "Authorization: XBL3.0 x=%s;%s", g.uhs, g.web_xsts);
    const char *headers[] = {
        xbl_auth, "x-xbl-contract-version: 4", "skillplatform: RemoteManagement",
        "Accept: application/json", "Accept-Language: en-US", "Content-Type: application/json"
    };
    HttpResponse r;
    if (!http_request("GET", "https://xccs.xboxlive.com/lists/devices?queryCurrentDevice=false&includeStorageDevices=true",
                      XC_UA, headers, ARRAY_SIZE(headers), NULL, 256 * 1024, &r)) {
        diagnostic_log("XHOME", "xccs devices request failed: %s", r.error);
        if (had_error && !primary_ok) *had_error = true;
        if (primary_ok) xbox_consoles_set(list, 0);
        return 0;
    }
    json_t *root = r.status == 200 ? response_json(&r) : NULL;
    diagnostic_log("XHOME", "xccs devices http=%ld", r.status);
    if (r.status != 200 || !root) {
        if (had_error && !primary_ok) {
            *had_error = true;
            snprintf(c->status, sizeof(c->status), i18n_text("Xbox console list: HTTP %ld"), r.status);
        }
        if (primary_ok) xbox_consoles_set(list, 0);
        json_decref(root);
        http_response_free(&r);
        return 0;
    }

    json_t *item;
    size_t index;
    json_array_foreach(json_object_get(root, "result"), index, item) {
        const char *id = json_string_value(json_object_get(item, "id"));
        const char *name = json_string_value(json_object_get(item, "name"));
        const char *type = json_string_value(json_object_get(item, "consoleType"));
        const char *power = json_string_value(json_object_get(item, "powerState"));
        if (!type || !type[0]) continue;
        json_t *st = json_object_get(item, "consoleStreamingEnabled");
        const bool streaming = !json_is_false(st);
        const bool remote_mgmt = !json_is_false(json_object_get(item, "remoteManagementEnabled"));
        const bool wireless = json_is_true(json_object_get(item, "wirelessWarning"));
        const bool out_of_home = json_is_true(json_object_get(item, "outOfHomeWarning"));
        added += append_console_entry(c, list, &console_total, id, name, type, power,
                                      streaming, remote_mgmt, wireless, out_of_home);
    }
    xbox_consoles_set(list, console_total);
    json_decref(root);
    http_response_free(&r);
    return added;
}

static void uuid_local(char out[40])
{
    static const char hex[] = "0123456789abcdef";
    int k = 0;
    for (int i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) out[k++] = '-';
        else out[k++] = hex[(rand() ^ (int)(plat_ticks() >> (i % 7))) & 15];
    }
    out[k] = '\0';
}

bool xcloud_wake_console(GfnClient *c, const char *console_id)
{
    if (!console_id || !console_id[0]) return false;
    if (!ensure_web_xsts(c)) return false;
    char session_uuid[40];
    uuid_local(session_uuid);
    json_t *req = json_pack("{s:s,s:s,s:s,s:s,s:s,s:[],s:s}",
                            "destination", "Xbox", "type", "Power", "command", "WakeUp",
                            "sessionId", session_uuid, "sourceId", "com.microsoft.smartglass",
                            "parameters", "linkedXboxId", console_id);
    char *body = req ? json_dumps(req, JSON_COMPACT) : NULL;
    json_decref(req);
    if (!body) return false;
    static char xbl_auth[4300];
    snprintf(xbl_auth, sizeof(xbl_auth), "Authorization: XBL3.0 x=%s;%s", g.uhs, g.web_xsts);
    const char *headers[] = {
        xbl_auth, "x-xbl-contract-version: 4", "skillplatform: RemoteManagement",
        "Accept: application/json", "Accept-Language: en-US", "Content-Type: application/json"
    };
    HttpResponse r;
    const bool sent = http_request("POST", "https://xccs.xboxlive.com/commands", XC_UA,
                                   headers, ARRAY_SIZE(headers), body, 64 * 1024, &r);
    free(body);
    if (!sent) {
        snprintf(c->status, sizeof(c->status), i18n_text("Couldn't reach Xbox console (%.80s)"), r.error);
        return false;
    }
    const long status = r.status;
    http_response_free(&r);
    diagnostic_log("XHOME", "wake %s http=%ld", console_id, status);
    if (status >= 200 && status < 300) {
        snprintf(c->status, sizeof(c->status), i18n_text("Wake-up sent to your Xbox; wait a few seconds."));
        return true;
    }
    snprintf(c->status, sizeof(c->status), i18n_text("Couldn't wake Xbox (HTTP %ld)"), status);
    return false;
}

bool xcloud_fetch_library(GfnClient *c)
{
    g.home_session = false;
    g_home_session = false;
    if (!xcloud_keep_login(c)) return false;
    snprintf(c->status, sizeof(c->status), i18n_text("Loading your Xbox consoles and games..."));
    c->game_count = 0;
    bool console_err = false;
    const size_t console_count = fetch_home_consoles(c, &console_err);
    size_t seen = 0;

    if (g.has_cloud && g.base[0] && c->access_token[0]) {
        XcTitle *titles = calloc(GFN_MAX_GAMES, sizeof(XcTitle));
        if (!titles) return console_count > 0;
        size_t count = 0;
        char continuation[512] = "";
        for (int page = 0; page < XC_TITLE_PAGES; ++page) {
            char path[896];
            char *encoded = continuation[0] ? http_url_encode(continuation) : NULL;
            const int pn = snprintf(path, sizeof(path), "/v2/titles?mr=500%s%s",
                                    encoded ? "&ct=" : "", encoded ? encoded : "");
            free(encoded);
            if (pn < 0 || (size_t)pn >= sizeof(path)) break;
            HttpResponse r;
            if (!gs_request(c, "GET", path, NULL, &r)) {
                free(titles);
                if (console_count > 0) break;
                return false;
            }
            json_t *root = r.status == 200 ? response_json(&r) : NULL;
            if (!root) {
                snprintf(c->status, sizeof(c->status), i18n_text("Xbox game list: HTTP %ld"), r.status);
                http_response_free(&r);
                free(titles);
                if (console_count > 0) break;
                return false;
            }
            size_t index;
            json_t *item;
            json_array_foreach(json_object_get(root, "results"), index, item) {
                ++seen;
                json_t *details = json_object_get(item, "details");
                if (!json_is_true(json_object_get(details, "hasEntitlement")) ||
                    c->game_count + count >= GFN_MAX_GAMES) continue;
                const char *id = json_string_value(json_object_get(item, "titleId"));
                const char *product = json_string_value(json_object_get(details, "productId"));
                if (!id || strlen(id) >= sizeof(titles[0].title_id)) continue;
                snprintf(titles[count].title_id, sizeof(titles[0].title_id), "%s", id);
                snprintf(titles[count].product_id, sizeof(titles[0].product_id), "%s", product ? product : "");
                ++count;
            }
            const char *next = json_string_value(json_object_get(root, "continuationToken"));
            if (next && strlen(next) < sizeof(continuation)) snprintf(continuation, sizeof(continuation), "%s", next);
            else continuation[0] = '\0';
            json_decref(root);
            http_response_free(&r);
            if (!continuation[0]) break;
        }
        for (size_t i = 0; i < count && c->game_count < GFN_MAX_GAMES; ++i) {
            GfnGame *game = &c->games[c->game_count++];
            memset(game, 0, sizeof(*game));
            /* Until the catalog names it. */
            snprintf(game->title, sizeof(game->title), "%s", titles[i].title_id);
            snprintf(game->app_id, sizeof(game->app_id), "%s", titles[i].title_id);
            snprintf(game->store, sizeof(game->store), "Xbox");
        }
        fill_names(c, titles, count);
        free(titles);
        if (c->game_count > console_count)
            qsort(&c->games[console_count], c->game_count - console_count, sizeof(GfnGame), compare_games);
    }

    if (c->game_count == 0 && !g.has_cloud) {
        if (!console_err)
            snprintf(c->status, sizeof(c->status),
                     i18n_text("No streamable Xbox consoles found (enable Remote Play on your console)."));
        return false;
    }
    c->catalog_total = c->game_count;
    diagnostic_log("XCLOUD", "library %lu consoles + %lu cloud games (%lu seen, %s)",
                   (unsigned long)console_count, (unsigned long)(c->game_count - console_count),
                   (unsigned long)seen, g.offering[0] ? g.offering : "xhome");
    if (console_count > 0 && c->game_count == console_count)
        snprintf(c->status, sizeof(c->status), i18n_text("Library: %lu Xbox console(s) (Y for consoles)"),
                 (unsigned long)console_count);
    else if (console_count > 0)
        snprintf(c->status, sizeof(c->status), i18n_text("Library: %lu console(s) + %lu cloud games (Y for consoles)"),
                 (unsigned long)console_count, (unsigned long)(c->game_count - console_count));
    else
        snprintf(c->status, sizeof(c->status), i18n_text("Library: %lu Xbox cloud games (Y for consoles)"),
                 (unsigned long)c->game_count);
    return true;
}

static bool contains_nocase(const char *text, const char *needle)
{
    const size_t n = strlen(needle);
    for (; *text; ++text)
        if (!strncasecmp(text, needle, n)) return true;
    return n == 0;
}

bool xcloud_search(GfnClient *c, const char *query)
{
    if (!gfn_library_load(c)) return false;
    size_t kept = 0;
    for (size_t i = 0; i < c->game_count; ++i) {
        if (!contains_nocase(c->games[i].title, query)) continue;
        if (kept != i) c->games[kept] = c->games[i];
        ++kept;
    }
    c->game_count = kept;
    snprintf(c->status, sizeof(c->status), i18n_text("%lu games match \"%.40s\""), (unsigned long)kept, query);
    return true;
}

/* ---- Sessions ------------------------------------------------------------------- */

bool xcloud_start_session(GfnClient *c, const GfnGame *game)
{
    const bool home = xcloud_is_home_store(game->store);
    g.home_session = home;
    g_home_session = home;
    /* Fortnite switches to 800x480 a few seconds in: keep it plain. */
    g_decode_720p = home || strcmp(game->app_id, "FORTNITE") != 0;
    diagnostic_log(home ? "XHOME" : "XCLOUD", "720p at half size: %s",
                   g_decode_720p ? "yes" : "no (Fortnite)");
    c->session_state = GFN_SESSION_IDLE;
    c->fail_code[0] = c->end_error_code[0] = '\0';
    c->queue_position = 0;
    if (!xcloud_keep_login(c)) {
        snprintf(c->fail_code, sizeof(c->fail_code), "login");
        c->session_state = GFN_SESSION_ERROR;
        return false;
    }
    if (home && (!g.home_token[0] || !g.home_base[0])) {
        snprintf(c->status, sizeof(c->status), i18n_text("Xbox Remote Play isn't available for this account."));
        snprintf(c->fail_code, sizeof(c->fail_code), "login");
        c->session_state = GFN_SESSION_ERROR;
        return false;
    }
    if (!home && (!g.has_cloud || !c->access_token[0] || !g.base[0])) {
        snprintf(c->status, sizeof(c->status), i18n_text("Xbox Cloud Gaming isn't available for this account."));
        snprintf(c->fail_code, sizeof(c->fail_code), "login");
        c->session_state = GFN_SESSION_ERROR;
        return false;
    }
    json_t *request = json_pack("{s:s,s:s,s:s,s:{s:s,s:b,s:b,s:i,s:s,s:b,s:i,s:s,s:s},s:s,s:[]}",
                                "clientSessionId", "",
                                "titleId", home ? "" : game->app_id,
                                "systemUpdateGroup", "",
                                "settings", "nanoVersion", "V3;WebrtcTransport.dll",
                                "enableOptionalDataCollection", 0, "enableTextToSpeech", 0, "highContrast", 0,
                                "locale", "en-US", "useIceConnection", 0, "timezoneOffsetMinutes", 0,
                                "sdkType", "web", "osName", "android",
                                "serverId", home ? game->app_id : "",
                                "fallbackRegionNames");
    char *text = request ? json_dumps(request, JSON_COMPACT) : NULL;
    json_decref(request);
    if (!text) return false;
    HttpResponse r;
    const bool sent = gs_request(c, "POST", home ? "/v5/sessions/home/play" : "/v5/sessions/cloud/play",
                                 text, &r);
    free(text);
    if (!sent) {
        c->session_state = GFN_SESSION_ERROR;
        return false;
    }
    json_t *root = response_json(&r);
    const char *path = json_string_value(json_object_get(root, "sessionPath"));
    diagnostic_log(home ? "XHOME" : "XCLOUD", "play %s http=%ld", game->app_id, r.status);
    if ((r.status != 200 && r.status != 202) || !path || strlen(path) >= sizeof(c->session_id)) {
        char why[120];
        error_text(&r, why, sizeof(why));
        snprintf(c->status, sizeof(c->status), i18n_text("Xbox couldn't start %.40s (HTTP %ld%s%.60s)"), game->title, r.status,
                 why[0] ? ", " : "", why);
        snprintf(c->fail_code, sizeof(c->fail_code), r.status == 403 ? "entitlement" : "create");
        diagnostic_log(home ? "XHOME" : "XCLOUD", "play refused: %.120s", why);
        json_decref(root);
        http_response_free(&r);
        c->session_state = GFN_SESSION_ERROR;
        return false;
    }
    snprintf(c->session_id, sizeof(c->session_id), "%s", path);
    snprintf(c->session_base_url, sizeof(c->session_base_url), "%s", session_base());
    {
        char subpath[200];
        xcloud_session_subpath(subpath, sizeof(subpath), path, NULL);
        snprintf(c->signaling_url, sizeof(c->signaling_url), "%s%s", session_base(), subpath);
    }
    json_decref(root);
    http_response_free(&r);
    c->session_state = GFN_SESSION_SETUP;
    g.connect_sent = false;
    g.next_state_at = 0;
    g.keepalive_failures = 0;
    g.last_state[0] = '\0';
    g.started_at = plat_time_ms();
    g.provisioning_at = 0;
    g.wait_seconds = 0;
    XboxConsole info;
    if (home && xbox_consoles_find(game->app_id, &info) && info.power == XBC_POWER_STANDBY)
        snprintf(c->status, sizeof(c->status), i18n_text("Waking up %.40s (about 30 s)..."), info.name);
    else
        snprintf(c->status, sizeof(c->status), home
                 ? i18n_text("Connecting to your Xbox console...")
                 : i18n_text("Asking Xbox Cloud Gaming for a console..."));
    return true;
}

/* The token /connect takes: a Microsoft "transfer" token for the console. */
static bool connect_session(GfnClient *c)
{
    char *encoded = http_url_encode(c->refresh_token);
    if (!encoded) return false;
    const size_t length = strlen(encoded) + 256;
    char *form = malloc(length);
    if (!form) {
        free(encoded);
        return false;
    }
    snprintf(form, length, "client_id=%s&grant_type=refresh_token&refresh_token=%s"
             "&scope=service::http://Passport.NET/purpose::PURPOSE_XBOX_CLOUD_CONSOLE_TRANSFER_TOKEN",
             XC_CLIENT_ID, encoded);
    free(encoded);
    static const char *const headers[] = {
        "Accept: application/json", "Content-Type: application/x-www-form-urlencoded"
    };
    HttpResponse r;
    const bool sent = http_request("POST", "https://login.live.com/oauth20_token.srf", XC_UA, headers,
                                   ARRAY_SIZE(headers), form, 256 * 1024, &r);
    free(form);
    if (!sent) return false;
    json_t *root = response_json(&r);
    const char *token = json_string_value(json_object_get(root, "access_token"));
    /* Do NOT overwrite c->refresh_token with the transfer-scoped refresh_token,
     * which has a different audience and breaks subsequent MSA token refreshes. */
    diagnostic_log("XCLOUD", "transfer token http=%ld %s", r.status, token ? "ok" : "missing");
    json_t *request = token ? json_pack("{s:s}", "userToken", token) : NULL;
    json_decref(root);
    http_response_free(&r);
    char *text = request ? json_dumps(request, JSON_COMPACT) : NULL;
    json_decref(request);
    if (!text) return false;
    char path[200];
    xcloud_session_subpath(path, sizeof(path), c->session_id, "connect");
    const bool posted = gs_request(c, "POST", path, text, &r);
    free(text);
    if (!posted) return false;
    diagnostic_log("XCLOUD", "connect http=%ld", r.status);
    const bool ok = r.status >= 200 && r.status < 300;
    http_response_free(&r);
    return ok;
}

/* The service's estimate of the wait in the queue, in seconds (0: none). */
static unsigned wait_estimate(GfnClient *c)
{
    char path[200];
    xcloud_session_subpath(path, sizeof(path), c->session_id, "waittime");
    HttpResponse r;
    if (!gs_request(c, "GET", path, NULL, &r)) return 0;
    json_t *root = r.status == 200 ? response_json(&r) : NULL;
    const json_int_t seconds = integer(root, "estimatedTotalWaitTimeInSeconds");
    diagnostic_log("XCLOUD", "queue estimate http=%ld %llds", r.status, (long long)seconds);
    json_decref(root);
    http_response_free(&r);
    return seconds > 0 && seconds < 6 * 3600 ? (unsigned)seconds : 0;
}

static void session_failed(GfnClient *c, const char *why, const char *code)
{
    c->session_state = GFN_SESSION_ERROR;
    snprintf(c->fail_code, sizeof(c->fail_code), "%s", code);
    snprintf(c->status, sizeof(c->status), "%.159s", why);
    diagnostic_log("XCLOUD", "session failed (%s): %.120s", code, why);
}

void xcloud_session_tick(GfnClient *c)
{
    if (!c->session_id[0]) return;
    const u64 now = plat_time_ms();
    if (c->session_state == GFN_SESSION_READY) {
        /* The service ends a session that hears nothing for a while. */
        if (now < g.next_keepalive_at) return;
        g.next_keepalive_at = now + 30000;
        char path[200];
        xcloud_session_subpath(path, sizeof(path), c->session_id, "keepalive");
        HttpResponse r;
        if (!gs_request(c, "POST", path, "", &r)) return;
        const long status = r.status;
        http_response_free(&r);
        if (status == 404 || status == 410 || status == 401 || status == 403 || status >= 500) {
            diagnostic_log("XCLOUD", "keepalive http=%ld", status);
            if (++g.keepalive_failures >= 2)
                session_failed(c, g.home_session ? "Your Xbox console ended the session."
                                                 : "Xbox Cloud Gaming ended the session.", "ended");
        } else {
            g.keepalive_failures = 0;
        }
        return;
    }
    if (c->session_state != GFN_SESSION_SETUP && c->session_state != GFN_SESSION_QUEUED) return;
    if (now < g.next_state_at) return;
    g.next_state_at = now + 1000;
    char path[200];
    xcloud_session_subpath(path, sizeof(path), c->session_id, "state");
    HttpResponse r;
    if (!gs_request(c, "GET", path, NULL, &r)) return;
    json_t *root = response_json(&r);
    const char *state = json_string_value(json_object_get(root, "state"));
    bool changed = false;
    if (state && strcmp(g.last_state, state)) {
        diagnostic_log("XCLOUD", "session state %s after %llus", state,
                       (unsigned long long)((now - g.started_at) / 1000));
        snprintf(g.last_state, sizeof(g.last_state), "%s", state);
        changed = true;
    }
    if (r.status == 404 || r.status == 401 || r.status == 403) {
        session_failed(c, "The Xbox session is gone. Try again.", "gone");
    } else if (g.provisioning_at && now - g.provisioning_at > XC_PROVISION_LIMIT_MS) {
        /* Seen as WaitingForResources (xHome), Provisioning or ReadyToConnect for minutes. */
        session_failed(c, g.home_session
                       ? "Your Xbox didn't wake up in time. Check that it's plugged in and Remote Play is on."
                       : "The Xbox didn't finish starting. Try again in a moment.", "slow-start");
    } else if (!state) {
        if (g.started_at && now - g.started_at > XC_UNKNOWN_LIMIT_MS)
            session_failed(c, "The Xbox stopped answering. Try again.", "timeout");
    } else if (!strcmp(state, "WaitingForResources")) {
        if (g.home_session) {
            /* On xHome, WaitingForResources means the console is waking from standby. */
            c->session_state = GFN_SESSION_SETUP;
            if (!g.provisioning_at) g.provisioning_at = now;
            snprintf(c->status, sizeof(c->status), i18n_text("Waking up your Xbox..."));
        } else {
            c->session_state = GFN_SESSION_QUEUED;
            if (changed) g.wait_seconds = wait_estimate(c);
            if (g.wait_seconds >= 60)
                snprintf(c->status, sizeof(c->status), i18n_text("Waiting for a free Xbox (about %u min)..."),
                         (g.wait_seconds + 30) / 60);
            else
                snprintf(c->status, sizeof(c->status), i18n_text("Waiting for a free Xbox..."));
        }
    } else if (!strcmp(state, "Provisioning")) {
        c->session_state = GFN_SESSION_SETUP;
        if (!g.provisioning_at) g.provisioning_at = now;
        snprintf(c->status, sizeof(c->status), g.home_session
                 ? i18n_text("Waking up your Xbox...") : i18n_text("Starting the Xbox..."));
    } else if (!strcmp(state, "ReadyToConnect")) {
        if (!g.provisioning_at) g.provisioning_at = now;
        if (!g.connect_sent) {
            snprintf(c->status, sizeof(c->status), i18n_text("Connecting your account to the Xbox..."));
            g.connect_sent = connect_session(c);
            if (!g.connect_sent) session_failed(c, "The Xbox didn't accept the sign-in. Try again.", "connect");
        }
    } else if (!strcmp(state, "Provisioned")) {
        c->session_state = GFN_SESSION_READY;
        g.provisioning_at = 0;
        g.next_keepalive_at = now + 30000;
        snprintf(c->status, sizeof(c->status), i18n_text("Xbox ready; connecting the stream..."));
    } else if (!strcmp(state, "Failed")) {
        char why[120];
        error_text(&r, why, sizeof(why));
        char text[160];
        const char *internal = strstr(why, "InternalCode :");
        if (internal && strstr(why, "ServerError")) {
            const unsigned long hresult = (unsigned long)(uint32_t)strtol(internal + 14, NULL, 10);
            snprintf(text, sizeof(text),
                     i18n_text("Microsoft couldn't start the game (error 0x%08lX). Try again soon, or test xbox.com/play "
                               "with the same account."),
                     hresult);
        } else {
            snprintf(text, sizeof(text), g.home_session
                     ? i18n_text("Xbox Remote Play couldn't connect%s%.110s")
                     : i18n_text("Xbox Cloud Gaming couldn't start the game%s%.110s"),
                     why[0] ? ": " : ".", why);
        }
        session_failed(c, text, "failed");
    } else if (g.started_at && now - g.started_at > XC_UNKNOWN_LIMIT_MS) {
        session_failed(c, "The Xbox session didn't start. Try again.", "state-timeout");
    }
    json_decref(root);
    http_response_free(&r);
}

bool xcloud_recover_session(GfnClient *c)
{
    if (!c->session_id[0]) return false;
    char path[200];
    xcloud_session_subpath(path, sizeof(path), c->session_id, "state");
    HttpResponse r;
    if (!gs_request(c, "GET", path, NULL, &r)) return false;
    json_t *root = response_json(&r);
    const char *state = json_string_value(json_object_get(root, "state"));
    const bool alive = r.status == 200 && state &&
                       (!strcmp(state, "Provisioned") || !strcmp(state, "ReadyToConnect") ||
                        !strcmp(state, "Provisioning") ||
                        (g.home_session && !strcmp(state, "WaitingForResources")));
    /* state points into root: decide before it is freed. */
    const bool provisioned = alive && !strcmp(state, "Provisioned");
    diagnostic_log("XCLOUD", "recover: http=%ld state=%s", r.status, state ? state : "-");
    json_decref(root);
    http_response_free(&r);
    if (alive) {
        if (provisioned) {
            c->session_state = GFN_SESSION_READY;
            g.provisioning_at = 0;
        } else {
            const u64 now = plat_time_ms();
            c->session_state = GFN_SESSION_SETUP;
            g.started_at = now;
            g.provisioning_at = now;
            g.connect_sent = false;
        }
        return true;
    }
    session_failed(c, "The Xbox session ended. Start the stream again.", "ended");
    return false;
}

bool xcloud_stop_session(GfnClient *c)
{
    g_decode_720p = false;
    if (!c->session_id[0]) {
        c->session_state = GFN_SESSION_IDLE;
        return true;
    }
    char path[200];
    xcloud_session_subpath(path, sizeof(path), c->session_id, NULL);
    HttpResponse r;
    if (!gs_request(c, "DELETE", path, NULL, &r)) {
        diagnostic_log("XCLOUD", "stop session unsent; left to expire");
    } else {
        diagnostic_log("XCLOUD", "stop session http=%ld", r.status);
        http_response_free(&r);
    }
    /* Always clear local session state so a stale 401/500 never blocks a restart. */
    memset(c->session_id, 0, sizeof(c->session_id));
    c->session_state = GFN_SESSION_IDLE;
    snprintf(c->status, sizeof(c->status), i18n_text("Xbox session ended"));
    return true;
}

/* ---- Offer, answer and ICE over HTTPS -------------------------------------------- */

/* GET until the service has the answer (204 while it works on it). */
static json_t *poll_exchange(const char *base, const char *token, const char *path,
                             unsigned timeout_ms, char *err_out, size_t err_size)
{
    const u64 deadline = plat_time_ms() + timeout_ms;
    while (plat_time_ms() < deadline) {
        HttpResponse r;
        if (!gs_request_raw(base, token, "GET", path, NULL, &r, err_out, err_size)) return NULL;
        if (r.status == 200) {
            json_t *root = response_json(&r);
            http_response_free(&r);
            const char *inner = json_string_value(json_object_get(root, "exchangeResponse"));
            json_error_t error;
            json_t *exchange = inner ? json_loads(inner, 0, &error) : NULL;
            if (!exchange) {
                char why[120] = "";
                json_t *details = json_object_get(root, "errorDetails");
                const char *message = json_string_value(json_object_get(details, "message"));
                snprintf(why, sizeof(why), "%s", message ? message : i18n_text("no answer"));
                snprintf(err_out, err_size, i18n_text("Xbox stream setup failed: %.100s"), why);
            }
            json_decref(root);
            return exchange;
        }
        const long status = r.status;
        http_response_free(&r);
        if (status != 204 && status != 429 && status < 500) {
            snprintf(err_out, err_size, i18n_text("Xbox stream setup: HTTP %ld"), status);
            return NULL;
        }
        plat_sleep_ns(400000000LL);
    }
    snprintf(err_out, err_size, i18n_text("Xbox stream setup timed out"));
    return NULL;
}

static bool signal_fail(NvstSignal *s, void *peer, const char *why)
{
    if (peer) webrtc_transport_discard_peer(peer);
    s->state = NVST_SIGNAL_ERROR;
    snprintf(s->status, sizeof(s->status), "%.159s", why);
    diagnostic_log("XCLOUD", "signalling failed: %.140s", why);
    return false;
}

/* SDP lines end in CRLF for libpeer's parser. */
static char *crlf(const char *sdp)
{
    char *out = malloc(strlen(sdp) * 2 + 3);
    if (!out) return NULL;
    char *o = out;
    for (const char *p = sdp; *p; ++p) {
        if (*p == '\n' && (p == sdp || p[-1] != '\r')) *o++ = '\r';
        *o++ = *p;
    }
    if (o > out && o[-1] != '\n') {
        *o++ = '\r';
        *o++ = '\n';
    }
    *o = '\0';
    return out;
}

static void add_ice_candidate(NvstSignal *s, const char *cand)
{
    if (!cand || s->remote_ice_count >= ARRAY_SIZE(s->remote_ice)) return;
    json_t *entry = json_pack("{s:s}", "candidate", cand);
    char *line = entry ? json_dumps(entry, JSON_COMPACT) : NULL;
    json_decref(entry);
    if (line && strlen(line) < sizeof(s->remote_ice[0])) {
        bool dup = false;
        for (unsigned i = 0; i < s->remote_ice_count; ++i) {
            if (!strcmp(s->remote_ice[i], line)) { dup = true; break; }
        }
        if (!dup) snprintf(s->remote_ice[s->remote_ice_count++], sizeof(s->remote_ice[0]), "%s", line);
    }
    free(line);
}

bool xcloud_signal_start(NvstSignal *s, const GfnClient *client)
{
    nvst_signal_close(s);
    s->xcloud = true;
    s->started_ms = plat_time_ms();
    s->state = NVST_SIGNAL_WAITING;
    snprintf(s->status, sizeof(s->status), i18n_text("Setting up the Xbox stream..."));
    login_load();

    const char *base = session_base();
    const char *token = session_token(client);
    char session_id[sizeof(client->session_id)];
    snprintf(session_id, sizeof(session_id), "%s", client->session_id);
    char err[160] = "";

    char *offer = NULL;
    void *peer = webrtc_transport_xcloud_offer(&offer);
    if (!peer || !offer) {
        free(offer);
        return signal_fail(s, peer, "Couldn't prepare the stream (WebRTC)");
    }
    static const char *const config =
        "{\"chatConfiguration\":{\"bytesPerSample\":2,\"expectedClipDurationMs\":20,\"format\":{\"codec\":\"opus\","
        "\"container\":\"webm\"},\"numChannels\":1,\"sampleFrequencyHz\":24000},\"chat\":{\"minVersion\":1,"
        "\"maxVersion\":1},\"control\":{\"minVersion\":1,\"maxVersion\":3},\"input\":{\"minVersion\":1,"
        "\"maxVersion\":9},\"message\":{\"minVersion\":1,\"maxVersion\":1}}";
    json_error_t error;
    json_t *request = json_pack("{s:s,s:s,s:s,s:o}", "messageType", "offer", "sdp", offer, "requestId", "1",
                                "configuration", json_loads(config, 0, &error));
    char *text = request ? json_dumps(request, JSON_COMPACT) : NULL;
    json_decref(request);
    char path[220];
    xcloud_session_subpath(path, sizeof(path), session_id, "sdp");
    HttpResponse r;
    const bool posted = text && gs_request_raw(base, token, "POST", path, text, &r, err, sizeof(err));
    free(text);
    if (!posted) {
        free(offer);
        return signal_fail(s, peer, err[0] ? err : "Couldn't send the stream offer");
    }
    const long offer_status = r.status;
    http_response_free(&r);
    if (offer_status < 200 || offer_status >= 300) {
        free(offer);
        char why[64];
        snprintf(why, sizeof(why), i18n_text("The Xbox refused the stream offer (HTTP %ld)"), offer_status);
        return signal_fail(s, peer, why);
    }
    json_t *answer = poll_exchange(base, token, path, 20000, err, sizeof(err));
    const char *answer_sdp = json_string_value(json_object_get(answer, "sdp"));
    const char *answer_status = json_string_value(json_object_get(answer, "status"));
    if (!answer_sdp || (answer_status && strcmp(answer_status, "success"))) {
        diagnostic_log("XCLOUD", "answer status=%s", answer_status ? answer_status : "-");
        json_decref(answer);
        free(offer);
        return signal_fail(s, peer, err[0] ? err : "The Xbox sent no stream answer");
    }
    s->offer_sdp = crlf(answer_sdp);
    s->offer_size = s->offer_sdp ? strlen(s->offer_sdp) : 0;
    json_decref(answer);
    diagnostic_log("XCLOUD", "answer %lu bytes", (unsigned long)s->offer_size);

    /* Our candidates (host, and server-reflexive when STUN answered). */
    json_t *candidates = json_array();
    for (const char *p = offer; (p = strstr(p, "a=candidate:")) != NULL;) {
        const char *end = strstr(p, "\r\n");
        const size_t n = end ? (size_t)(end - p) : strlen(p);
        char line[256];
        snprintf(line, sizeof(line), "%.*s", (int)(n < sizeof(line) ? n : sizeof(line) - 1), p);
        json_array_append_new(candidates, json_pack("{s:s,s:s,s:s,s:s}", "candidate", line, "messageType",
                                                    "iceCandidate", "sdpMLineIndex", "0", "sdpMid", "0"));
        p += n;
    }
    json_array_append_new(candidates, json_pack("{s:s,s:s,s:s,s:s}", "candidate", "a=end-of-candidates",
                                                "messageType", "iceCandidate", "sdpMLineIndex", "0", "sdpMid", "0"));
    free(offer);
    diagnostic_log("XCLOUD", "local candidates %lu", (unsigned long)json_array_size(candidates) - 1);
    request = json_pack("{s:s,s:o}", "messageType", "iceCandidate", "candidate", candidates);
    text = request ? json_dumps(request, JSON_COMPACT) : NULL;
    json_decref(request);
    xcloud_session_subpath(path, sizeof(path), session_id, "ice");
    const bool sent = text && gs_request_raw(base, token, "POST", path, text, &r, err, sizeof(err));
    free(text);
    if (!sent) return signal_fail(s, peer, err[0] ? err : "Couldn't send the network candidates");
    http_response_free(&r);
    json_t *remote = poll_exchange(base, token, path, 15000, err, sizeof(err));
    size_t index;
    json_t *item;
    json_array_foreach(remote, index, item) {
        const char *candidate = json_string_value(json_object_get(item, "candidate"));
        if (!candidate || strstr(candidate, "end-of-candidates") || s->remote_ice_count >= ARRAY_SIZE(s->remote_ice))
            continue;
        /* IPv4 only: the console has no IPv6. For Xbox Remote Play, unwrap
         * Teredo IPv6 candidates (2001:0000::/32) into IPv4 host candidates. */
        char address[64] = "";
        unsigned cand_port = 0;
        const char *body = !strncmp(candidate, "a=candidate:", 12) ? candidate + 2 : candidate;
        sscanf(body, "candidate:%*s %*u %*s %*u %63s %u", address, &cand_port);
        if (strchr(address, ':')) {
            char ip4[32] = "";
            uint16_t tport = 0;
            if (xcloud_teredo_ipv4(address, ip4, sizeof(ip4), &tport)) {
                char cand[128];
                snprintf(cand, sizeof(cand), "a=candidate:10 1 UDP 1 %s 9002 typ host ", ip4);
                add_ice_candidate(s, cand);
                if (tport && tport != 9002) {
                    snprintf(cand, sizeof(cand), "a=candidate:11 1 UDP 1 %s %u typ host ", ip4, (unsigned)tport);
                    add_ice_candidate(s, cand);
                }
                if (cand_port && cand_port <= 65535 && cand_port != 9002 && cand_port != tport) {
                    snprintf(cand, sizeof(cand), "a=candidate:12 1 UDP 1 %s %u typ host ", ip4, cand_port);
                    add_ice_candidate(s, cand);
                }
            } else {
                diagnostic_log("XCLOUD", "ignored non-Teredo IPv6 candidate: %.63s", address);
            }
            continue;
        }
        add_ice_candidate(s, candidate);
    }
    json_decref(remote);
    if (g.home_session) {
        char cfg_path[220];
        xcloud_session_subpath(cfg_path, sizeof(cfg_path), session_id, "configuration");
        HttpResponse cfg_r;
        if (gs_request_raw(base, token, "GET", cfg_path, NULL, &cfg_r, NULL, 0)) {
            if (cfg_r.status == 200) {
                json_t *cfg = response_json(&cfg_r);
                json_t *sd = json_object_get(cfg, "serverDetails");
                const char *ip = json_string_value(json_object_get(sd, "ipV4Address"));
                if (!ip || !ip[0]) ip = json_string_value(json_object_get(sd, "ipAddress"));
                json_int_t port = integer(sd, "ipV4Port");
                if (port <= 0) port = integer(sd, "port");
                if (port <= 0) port = 9002;
                if (ip && ip[0] && !strchr(ip, ':') && port > 0 && port <= 65535) {
                    char cand[128];
                    snprintf(cand, sizeof(cand), "a=candidate:20 1 UDP 1 %s %lld typ host ", ip, (long long)port);
                    add_ice_candidate(s, cand);
                    diagnostic_log("XHOME", "config serverDetails IPv4 %s:%lld", ip, (long long)port);
                }
                json_decref(cfg);
            }
            http_response_free(&cfg_r);
        }
    }
    diagnostic_log("XCLOUD", "remote candidates %u (IPv4/Teredo)", s->remote_ice_count);
    if (!s->remote_ice_count || !s->offer_sdp) return signal_fail(s, peer, "The Xbox sent no usable address");
    s->xcloud_peer = peer;
    s->state = NVST_SIGNAL_OFFER;
    snprintf(s->status, sizeof(s->status), i18n_text("Connecting to the Xbox..."));
    return true;
}
