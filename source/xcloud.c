#include "xcloud.h"

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
#define XC_LIBRARY_PATH APP_DATA_DIR "/xcloud-library.json"
#define XC_RESPONSE_MAX (2 * 1024 * 1024)
/* Entitled titles are fetched in pages; the catalog names them in batches. */
#define XC_TITLE_PAGES 12
#define XC_CATALOG_BATCH 40

static struct {
    bool loaded;
    char base[128];     /* the region's session service, no trailing slash */
    char region[32];
    char offering[16];  /* xgpuweb (Game Pass) or xgpuwebf2p (free to play) */
    bool connect_sent;  /* /connect for the current session */
    u64 next_state_at, next_keepalive_at;
    unsigned keepalive_failures;
    /* The session's last state (logged on change), when it started, and
     * when its console started (Provisioning) for the start-up limit. */
    char last_state[32];
    u64 started_at, provisioning_at;
    /* The service's wait estimate while queued (0: none). */
    unsigned wait_seconds;
} g;

/* A console that never finishes starting: the session is given up. */
#define XC_PROVISION_LIMIT_MS 150000

static char g_auth_header[8300];
static volatile bool g_selected;

void xcloud_select(bool xbox) { g_selected = xbox; }
bool xcloud_selected(void) { return g_selected; }

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

/* The region from the saved login (the tokens go into the client). */
static json_t *login_file(void)
{
    json_error_t error;
    json_t *root = json_load_file(XC_LOGIN_PATH, 0, &error);
    if (json_is_object(root)) return root;
    json_decref(root);
    return NULL;
}

static void login_load(void)
{
    if (g.loaded) return;
    g.loaded = true;
    json_t *root = login_file();
    if (!root) return;
    copy_string(g.base, sizeof(g.base), root, "base");
    copy_string(g.region, sizeof(g.region), root, "region");
    copy_string(g.offering, sizeof(g.offering), root, "offering");
    json_decref(root);
    /* Only Microsoft's own session hosts. */
    if (strncmp(g.base, "https://", 8) || !strstr(g.base, ".gssv-play-prod.xboxlive.com")) g.base[0] = '\0';
}

static bool login_save(const GfnClient *c)
{
    json_t *root = json_pack("{s:s,s:s,s:s,s:s,s:s,s:I,s:s,s:I}", "base", g.base, "region", g.region,
                             "offering", g.offering, "refresh_token", c->refresh_token,
                             "msa_token", c->id_token, "msa_expires_at", (json_int_t)c->client_token_expires_at,
                             "stream_token", c->access_token, "stream_expires_at", (json_int_t)c->token_expires_at);
    const bool ok = root && json_dump_file(root, XC_LOGIN_PATH, JSON_COMPACT) == 0;
    json_decref(root);
    return ok;
}

bool xcloud_load_login(GfnClient *c)
{
    g.loaded = false;
    login_load();
    json_t *root = login_file();
    if (!root) return false;
    copy_string(c->refresh_token, sizeof(c->refresh_token), root, "refresh_token");
    copy_string(c->id_token, sizeof(c->id_token), root, "msa_token");
    copy_string(c->access_token, sizeof(c->access_token), root, "stream_token");
    c->client_token_expires_at = integer(root, "msa_expires_at");
    c->token_expires_at = integer(root, "stream_expires_at");
    json_decref(root);
    provider_xbox(&c->login_provider);
    /* The streaming token is renewed from the refresh token when needed. */
    if (!c->access_token[0] && c->refresh_token[0]) snprintf(c->access_token, sizeof(c->access_token), "renew");
    return c->refresh_token[0] && g.base[0];
}

bool xcloud_login_saved(void)
{
    json_t *root = login_file();
    const bool saved = root && json_string_value(json_object_get(root, "refresh_token"));
    json_decref(root);
    return saved;
}

void xcloud_sign_out(void)
{
    remove(XC_LOGIN_PATH);
    remove(XC_LIBRARY_PATH);
    memset(&g, 0, sizeof(g));
    g.loaded = true;
}

/* ---- Microsoft account ---------------------------------------------------------- */

static bool token_form(GfnClient *c, const char *form, const char **error_code, long *status)
{
    static const char *const headers[] = {
        "Accept: application/json", "Content-Type: application/x-www-form-urlencoded"
    };
    HttpResponse r;
    *error_code = "";
    *status = 0;
    if (!http_request("POST", XC_TOKEN_URL, XC_UA, headers, ARRAY_SIZE(headers), form, 256 * 1024, &r)) {
        snprintf(c->status, sizeof(c->status), "Can't reach Microsoft's sign-in (%.80s)", r.error);
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
    } else if (root) {
        static char code[48];
        copy_string(code, sizeof(code), root, "error");
        *error_code = code;
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
    const char *code;
    long status;
    const bool ok = token_form(c, form, &code, &status);
    free(form);
    diagnostic_log("XCLOUD", "microsoft refresh http=%ld %s%s", status, ok ? "ok" : "failed ", ok ? "" : code);
    if (!ok && (status == 400 || status == 401)) {
        c->auth_state = GFN_AUTH_ERROR;
        snprintf(c->status, sizeof(c->status), "Your Microsoft sign-in has expired. Press X to sign in again.");
    }
    return ok;
}

static const char *const g_xbl_headers[] = {
    "x-xbl-contract-version: 1", "Accept: application/json", "Content-Type: application/json",
    "Origin: https://www.xbox.com", "Referer: https://www.xbox.com/"
};

/* Microsoft token -> Xbox user token -> XSTS for the streaming service. */
static bool xsts_gssv(GfnClient *c, char *out, size_t size)
{
    char *body = malloc(strlen(c->id_token) + 256);
    if (!body) return false;
    sprintf(body, "{\"Properties\":{\"AuthMethod\":\"RPS\",\"RpsTicket\":\"d=%s\",\"SiteName\":\"user.auth.xboxlive.com\"},"
                  "\"RelyingParty\":\"http://auth.xboxlive.com\",\"TokenType\":\"JWT\"}", c->id_token);
    HttpResponse r;
    const bool sent = http_request("POST", "https://user.auth.xboxlive.com/user/authenticate", XC_UA,
                                   g_xbl_headers, ARRAY_SIZE(g_xbl_headers), body, 256 * 1024, &r);
    free(body);
    if (!sent) {
        snprintf(c->status, sizeof(c->status), "Can't reach Xbox Live (%.80s)", r.error);
        return false;
    }
    json_t *root = response_json(&r);
    const char *user = json_string_value(json_object_get(root, "Token"));
    diagnostic_log("XCLOUD", "user token http=%ld %s", r.status, user ? "ok" : "missing");
    if (r.status != 200 || !user) {
        snprintf(c->status, sizeof(c->status), "Xbox Live refused the sign-in (HTTP %ld)", r.status);
        json_decref(root);
        http_response_free(&r);
        return false;
    }
    json_t *request = json_pack("{s:{s:s,s:[s]},s:s,s:s}", "Properties", "SandboxId", "RETAIL", "UserTokens", user,
                                "RelyingParty", "http://gssv.xboxlive.com/", "TokenType", "JWT");
    json_decref(root);
    http_response_free(&r);
    char *text = request ? json_dumps(request, JSON_COMPACT) : NULL;
    json_decref(request);
    if (!text) return false;
    const bool sent2 = http_request("POST", "https://xsts.auth.xboxlive.com/xsts/authorize", XC_UA,
                                    g_xbl_headers, ARRAY_SIZE(g_xbl_headers), text, 256 * 1024, &r);
    free(text);
    if (!sent2) {
        snprintf(c->status, sizeof(c->status), "Can't reach Xbox Live (%.80s)", r.error);
        return false;
    }
    root = response_json(&r);
    const char *xsts = json_string_value(json_object_get(root, "Token"));
    /* XErr 2148916233: no Xbox profile yet; 2148916238: a child account. */
    const json_int_t xerr = json_integer_value(json_object_get(root, "XErr"));
    diagnostic_log("XCLOUD", "xsts http=%ld %s xerr=%lld", r.status, xsts ? "ok" : "missing", (long long)xerr);
    bool ok = r.status == 200 && xsts && strlen(xsts) < size;
    if (ok) snprintf(out, size, "%s", xsts);
    else if (xerr == 2148916233LL)
        snprintf(c->status, sizeof(c->status), "This Microsoft account has no Xbox profile yet. Sign in once on "
                 "xbox.com to make one, then try again.");
    else if (xerr == 2148916238LL)
        snprintf(c->status, sizeof(c->status), "A child account needs a parent to allow cloud gaming first.");
    else
        snprintf(c->status, sizeof(c->status), "Xbox Live refused the sign-in (HTTP %ld)", r.status);
    json_decref(root);
    http_response_free(&r);
    return ok;
}

/* The streaming token for Game Pass, else for free-to-play games, and the
 * account's default region. */
static bool streaming_token(GfnClient *c)
{
    login_load();
    if (!msa_access(c)) return false;
    static char xsts[4096];
    if (!xsts_gssv(c, xsts, sizeof(xsts))) return false;
    static const char *const offerings[] = { "xgpuweb", "xgpuwebf2p" };
    for (size_t i = 0; i < ARRAY_SIZE(offerings); ++i) {
        json_t *request = json_pack("{s:s,s:s}", "token", xsts, "offeringId", offerings[i]);
        char *text = request ? json_dumps(request, JSON_COMPACT) : NULL;
        json_decref(request);
        if (!text) return false;
        char url[128];
        snprintf(url, sizeof(url), "https://%s.gssv-play-prod.xboxlive.com/v2/login/user", offerings[i]);
        static const char *const headers[] = {
            "Content-Type: application/json", "Accept: application/json", "x-gssv-client: XboxComBrowser",
            "Cache-Control: no-store, must-revalidate, no-cache"
        };
        HttpResponse r;
        const bool sent = http_request("POST", url, XC_UA, headers, ARRAY_SIZE(headers), text, 512 * 1024, &r);
        free(text);
        if (!sent) {
            snprintf(c->status, sizeof(c->status), "Can't reach Xbox Cloud Gaming (%.80s)", r.error);
            return false;
        }
        json_t *root = response_json(&r);
        const char *token = json_string_value(json_object_get(root, "gsToken"));
        diagnostic_log("XCLOUD", "streaming login %s http=%ld %s", offerings[i], r.status, token ? "ok" : "refused");
        if (r.status == 200 && token && strlen(token) < sizeof(c->access_token)) {
            snprintf(c->access_token, sizeof(c->access_token), "%s", token);
            json_t *duration = json_object_get(root, "durationInSeconds");
            c->token_expires_at = (int64_t)time(NULL) + (json_is_integer(duration) ? json_integer_value(duration) : 3600);
            snprintf(g.offering, sizeof(g.offering), "%s", offerings[i]);
            json_t *regions = json_object_get(json_object_get(root, "offeringSettings"), "regions");
            size_t index;
            json_t *region;
            char first[128] = "", first_name[32] = "";
            bool kept = false;
            json_array_foreach(regions, index, region) {
                const char *uri = json_string_value(json_object_get(region, "baseUri"));
                const char *name = json_string_value(json_object_get(region, "name"));
                if (!uri || !name || strncmp(uri, "https://", 8) || strlen(uri) >= sizeof(g.base)) continue;
                if (!first[0] || json_is_true(json_object_get(region, "isDefault"))) {
                    snprintf(first, sizeof(first), "%s", uri);
                    snprintf(first_name, sizeof(first_name), "%s", name);
                }
                /* Keep a region picked before while it is offered. */
                if (g.region[0] && !strcmp(g.region, name)) kept = true;
            }
            if (!kept && first[0]) {
                snprintf(g.base, sizeof(g.base), "%s", first);
                snprintf(g.region, sizeof(g.region), "%s", first_name);
            }
            size_t n = strlen(g.base);
            while (n > 8 && g.base[n - 1] == '/') g.base[--n] = '\0';
            diagnostic_log("XCLOUD", "region %s, %lu offered, token lasts %llds", g.region,
                           (unsigned long)json_array_size(regions),
                           (long long)(c->token_expires_at - (int64_t)time(NULL)));
            json_decref(root);
            http_response_free(&r);
            login_save(c);
            return g.base[0] != '\0';
        }
        json_decref(root);
        http_response_free(&r);
    }
    snprintf(c->status, sizeof(c->status), "Xbox Cloud Gaming isn't available for this account or country.");
    return false;
}

bool xcloud_begin_login(GfnClient *c)
{
    static const char *const headers[] = {
        "Accept: application/json", "Content-Type: application/x-www-form-urlencoded"
    };
    xcloud_sign_out();
    provider_xbox(&c->login_provider);
    snprintf(c->status, sizeof(c->status), "Contacting Microsoft sign-in...");
    HttpResponse r;
    if (!http_request("POST", "https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode", XC_UA,
                      headers, ARRAY_SIZE(headers), "client_id=" XC_CLIENT_ID "&scope=" XC_SCOPE, 64 * 1024, &r)) {
        char date[16];
        if (http_clock_wrong(date, sizeof(date)))
            snprintf(c->status, sizeof(c->status), "Your 3DS thinks it is %s, so the secure sign-in fails. Set the "
                     "date and time in System Settings, then try again.", date);
        else
            snprintf(c->status, sizeof(c->status), "Couldn't reach Microsoft's sign-in (%.60s). Check Wi-Fi.", r.error);
        c->auth_state = GFN_AUTH_ERROR;
        return false;
    }
    json_t *root = response_json(&r);
    if (r.status != 200 || !root) {
        snprintf(c->status, sizeof(c->status), "Microsoft sign-in: HTTP %ld", r.status);
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
        snprintf(c->status, sizeof(c->status), "Microsoft sign-in response incomplete");
        c->auth_state = GFN_AUTH_ERROR;
        return false;
    }
    c->auth_state = GFN_AUTH_WAITING;
    snprintf(c->status, sizeof(c->status), "Signing in to Xbox: open the link on a phone or PC and enter the code");
    diagnostic_log("XCLOUD", "device code issued, lasts %llds", (long long)(c->challenge_expires_at - now));
    return true;
}

void xcloud_login_tick(GfnClient *c)
{
    if (c->auth_state != GFN_AUTH_WAITING) return;
    const int64_t now = (int64_t)time(NULL);
    if (now >= c->challenge_expires_at) {
        c->auth_state = GFN_AUTH_ERROR;
        snprintf(c->status, sizeof(c->status), "Sign-in code expired; press X to get a new one");
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
    const char *code;
    long status;
    const bool ok = token_form(c, form, &code, &status);
    free(form);
    if (!ok) {
        if (!strcmp(code, "authorization_pending")) {
            snprintf(c->status, sizeof(c->status), "Waiting for the Microsoft sign-in...");
        } else if (!strcmp(code, "slow_down")) {
            c->poll_interval += 5;
        } else if (status) {
            diagnostic_log("XCLOUD", "sign-in poll http=%ld error=%s", status, code);
            c->auth_state = GFN_AUTH_ERROR;
            snprintf(c->status, sizeof(c->status), !strcmp(code, "authorization_declined")
                     ? "The sign-in was declined. Press X to try again." : "Microsoft sign-in failed (%.40s)", code);
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
    snprintf(c->status, sizeof(c->status), "Signed in to Xbox Cloud Gaming; %s", saved ? "saved to SD" : "save failed");
}

bool xcloud_keep_login(GfnClient *c)
{
    login_load();
    if (c->access_token[0] && g.base[0] && (int64_t)time(NULL) + 600 < c->token_expires_at) return true;
    if (!streaming_token(c)) {
        /* A passing failure keeps the token while it lasts. */
        return c->access_token[0] && (int64_t)time(NULL) + 60 < c->token_expires_at && g.base[0];
    }
    login_save(c);
    return true;
}

/* ---- Requests to the region's session service --------------------------------- */

static bool gs_request(GfnClient *c, const char *method, const char *path, const char *body, HttpResponse *r)
{
    char url[384];
    snprintf(url, sizeof(url), "%s%s", g.base, path);
    snprintf(g_auth_header, sizeof(g_auth_header), "Authorization: Bearer %s", c->access_token);
    /* The device info picks the stream: Microsoft fits the picture to the
     * client's screen (custom resolutions are allowed). */
    char info[900];
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
    if (!sent) snprintf(c->status, sizeof(c->status), "Xbox Cloud Gaming unreachable (%.80s)", r->error);
    return sent;
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

/* ---- Library ------------------------------------------------------------------- */

typedef struct {
    char title_id[48];
    char product_id[16];
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
                if (strcmp(game->app_id, titles[i].title_id)) continue;
                const char *name = json_string_value(json_object_get(p, "ProductTitle"));
                if (name && name[0]) snprintf(game->title, sizeof(game->title), "%s", name);
                const char *poster = json_string_value(json_object_get(json_object_get(p, "Image_Poster"), "URL"));
                const char *tile = json_string_value(json_object_get(json_object_get(p, "Image_Tile"), "URL"));
                const char *art = poster ? poster : tile;
                if (art && !strncmp(art, "//", 2))
                    snprintf(game->image_url, sizeof(game->image_url), "https:%s", art);
                if (tile && !strncmp(tile, "//", 2))
                    snprintf(game->wide_url, sizeof(game->wide_url), "https:%s", tile);
            }
        }
        json_decref(root);
        http_response_free(&r);
    }
}

bool xcloud_fetch_library(GfnClient *c)
{
    if (!xcloud_keep_login(c)) return false;
    snprintf(c->status, sizeof(c->status), "Loading your Xbox cloud games...");
    XcTitle *titles = calloc(GFN_MAX_GAMES, sizeof(XcTitle));
    if (!titles) return false;
    size_t count = 0, seen = 0;
    char continuation[512] = "";
    for (int page = 0; page < XC_TITLE_PAGES; ++page) {
        char path[640];
        char *encoded = continuation[0] ? http_url_encode(continuation) : NULL;
        snprintf(path, sizeof(path), "/v2/titles?mr=500%s%s", encoded ? "&ct=" : "", encoded ? encoded : "");
        free(encoded);
        HttpResponse r;
        if (!gs_request(c, "GET", path, NULL, &r)) {
            free(titles);
            return false;
        }
        json_t *root = r.status == 200 ? response_json(&r) : NULL;
        if (!root) {
            snprintf(c->status, sizeof(c->status), "Xbox game list: HTTP %ld", r.status);
            http_response_free(&r);
            free(titles);
            return false;
        }
        size_t index;
        json_t *item;
        json_array_foreach(json_object_get(root, "results"), index, item) {
            ++seen;
            json_t *details = json_object_get(item, "details");
            if (!json_is_true(json_object_get(details, "hasEntitlement")) || count >= GFN_MAX_GAMES) continue;
            const char *id = json_string_value(json_object_get(item, "titleId"));
            const char *product = json_string_value(json_object_get(details, "productId"));
            if (!id || strlen(id) >= sizeof(titles[0].title_id)) continue;
            snprintf(titles[count].title_id, sizeof(titles[0].title_id), "%s", id);
            snprintf(titles[count].product_id, sizeof(titles[0].product_id), "%s", product ? product : "");
            ++count;
        }
        const char *next = json_string_value(json_object_get(root, "continuationToken"));
        snprintf(continuation, sizeof(continuation), "%s", next && strlen(next) < sizeof(continuation) ? next : "");
        json_decref(root);
        http_response_free(&r);
        if (!continuation[0]) break;
    }
    c->game_count = 0;
    for (size_t i = 0; i < count; ++i) {
        GfnGame *game = &c->games[c->game_count++];
        memset(game, 0, sizeof(*game));
        /* Until the catalog names it. */
        snprintf(game->title, sizeof(game->title), "%s", titles[i].title_id);
        snprintf(game->app_id, sizeof(game->app_id), "%s", titles[i].title_id);
        snprintf(game->store, sizeof(game->store), "Xbox");
    }
    fill_names(c, titles, count);
    free(titles);
    qsort(c->games, c->game_count, sizeof(GfnGame), compare_games);
    c->catalog_total = c->game_count;
    diagnostic_log("XCLOUD", "library %lu playable of %lu cloud games (%s)", (unsigned long)c->game_count,
                   (unsigned long)seen, g.offering);
    snprintf(c->status, sizeof(c->status), "Library: %lu Xbox cloud games (Y refreshes)",
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
    snprintf(c->status, sizeof(c->status), "%lu games match \"%.40s\"", (unsigned long)kept, query);
    return true;
}

/* ---- Sessions ------------------------------------------------------------------- */

/* ---- Resolution experiments ---------------------------------------------------- */

static int g_experiment;

int xcloud_experiment(void) { return g_experiment; }

const char *xcloud_experiment_name(int experiment)
{
    switch (experiment) {
    case XCLOUD_TEST_720P_SHRINK: return "720p shrunk to 800x480, 1.5 Mbps (new)";
    case XCLOUD_TEST_720P_FULL: return "720p full size, 1.2 Mbps";
    default: return "none";
    }
}

/* The next experiment, kept on the SD card so a crash moves on to the next. */
static int next_experiment(void)
{
    const char *path = APP_DATA_DIR "/xbox-test.txt";
    int last = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        if (fscanf(f, "%d", &last) != 1) last = 0;
        fclose(f);
    }
    int next = last + 1;
    if (next <= XCLOUD_TEST_NONE || next >= XCLOUD_TEST_COUNT) next = XCLOUD_TEST_NONE + 1;
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%d\n", next);
        fclose(f);
    }
    return next;
}

bool xcloud_available(void)
{
    static int known = -1;
    if (known < 0) {
        FILE *f = fopen(APP_DATA_DIR "/xbox-test.txt", "r");
        known = f != NULL;
        if (f) fclose(f);
    }
    return known == 1;
}

bool xcloud_start_session(GfnClient *c, const GfnGame *game)
{
    /* Fortnite already streams 800x480: keep it plain. Tests only run on
     * a tester's console (xcloud_available). */
    g_experiment = xcloud_available() && strcmp(game->app_id, "FORTNITE") ? next_experiment()
                                                                            : XCLOUD_TEST_NONE;
    if (g_experiment)
        diagnostic_log("XCLOUD", "resolution test %d: %s", g_experiment, xcloud_experiment_name(g_experiment));
    c->session_state = GFN_SESSION_IDLE;
    c->fail_code[0] = c->end_error_code[0] = '\0';
    c->queue_position = 0;
    if (!xcloud_keep_login(c)) {
        snprintf(c->fail_code, sizeof(c->fail_code), "login");
        c->session_state = GFN_SESSION_ERROR;
        return false;
    }
    json_t *request = json_pack("{s:s,s:s,s:s,s:{s:s,s:b,s:b,s:i,s:s,s:b,s:i,s:s,s:s},s:s,s:[]}",
                                "clientSessionId", "", "titleId", game->app_id, "systemUpdateGroup", "",
                                "settings", "nanoVersion", "V3;WebrtcTransport.dll",
                                "enableOptionalDataCollection", 0, "enableTextToSpeech", 0, "highContrast", 0,
                                "locale", "en-US", "useIceConnection", 0, "timezoneOffsetMinutes", 0,
                                "sdkType", "web", "osName", "android",
                                "serverId", "", "fallbackRegionNames");
    char *text = request ? json_dumps(request, JSON_COMPACT) : NULL;
    json_decref(request);
    if (!text) return false;
    HttpResponse r;
    const bool sent = gs_request(c, "POST", "/v5/sessions/cloud/play", text, &r);
    free(text);
    if (!sent) {
        c->session_state = GFN_SESSION_ERROR;
        return false;
    }
    json_t *root = response_json(&r);
    const char *path = json_string_value(json_object_get(root, "sessionPath"));
    diagnostic_log("XCLOUD", "play %s http=%ld", game->app_id, r.status);
    if ((r.status != 200 && r.status != 202) || !path || strlen(path) >= sizeof(c->session_id)) {
        char why[120];
        error_text(&r, why, sizeof(why));
        snprintf(c->status, sizeof(c->status), "Xbox couldn't start %.40s (HTTP %ld%s%.60s)", game->title, r.status,
                 why[0] ? ", " : "", why);
        snprintf(c->fail_code, sizeof(c->fail_code), r.status == 403 ? "entitlement" : "create");
        diagnostic_log("XCLOUD", "play refused: %.120s", why);
        json_decref(root);
        http_response_free(&r);
        c->session_state = GFN_SESSION_ERROR;
        return false;
    }
    snprintf(c->session_id, sizeof(c->session_id), "%s", path);
    snprintf(c->session_base_url, sizeof(c->session_base_url), "%s", g.base);
    snprintf(c->signaling_url, sizeof(c->signaling_url), "%s/%s", g.base, path);
    json_decref(root);
    http_response_free(&r);
    c->session_state = GFN_SESSION_SETUP;
    g.connect_sent = false;
    g.next_state_at = 0;
    g.keepalive_failures = 0;
    g.last_state[0] = '\0';
    g.started_at = osGetTime();
    g.provisioning_at = 0;
    g.wait_seconds = 0;
    snprintf(c->status, sizeof(c->status), "Asking Xbox Cloud Gaming for a console...");
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
    const char *refresh = json_string_value(json_object_get(root, "refresh_token"));
    if (refresh && refresh[0]) {
        snprintf(c->refresh_token, sizeof(c->refresh_token), "%s", refresh);
        login_save(c);
    }
    diagnostic_log("XCLOUD", "transfer token http=%ld %s", r.status, token ? "ok" : "missing");
    json_t *request = token ? json_pack("{s:s}", "userToken", token) : NULL;
    json_decref(root);
    http_response_free(&r);
    char *text = request ? json_dumps(request, JSON_COMPACT) : NULL;
    json_decref(request);
    if (!text) return false;
    char path[200];
    snprintf(path, sizeof(path), "/%s/connect", c->session_id);
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
    snprintf(path, sizeof(path), "/%s/waittime", c->session_id);
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
    const u64 now = osGetTime();
    if (c->session_state == GFN_SESSION_READY) {
        /* The service ends a session that hears nothing for a while. */
        if (now < g.next_keepalive_at) return;
        g.next_keepalive_at = now + 30000;
        char path[200];
        snprintf(path, sizeof(path), "/%s/keepalive", c->session_id);
        HttpResponse r;
        if (!gs_request(c, "POST", path, "", &r)) return;
        const long status = r.status;
        http_response_free(&r);
        if (status == 404 || status == 410) {
            if (++g.keepalive_failures >= 2) session_failed(c, "Xbox Cloud Gaming ended the session.", "ended");
        } else {
            g.keepalive_failures = 0;
        }
        return;
    }
    if (c->session_state != GFN_SESSION_SETUP && c->session_state != GFN_SESSION_QUEUED) return;
    if (now < g.next_state_at) return;
    g.next_state_at = now + 1000;
    char path[200];
    snprintf(path, sizeof(path), "/%s/state", c->session_id);
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
    if (r.status == 404) {
        session_failed(c, "The Xbox session is gone. Try again.", "gone");
    } else if (!state) {
        /* A passing hiccup: the next poll tries again. */
    } else if (!strcmp(state, "WaitingForResources")) {
        c->session_state = GFN_SESSION_QUEUED;
        if (changed) g.wait_seconds = wait_estimate(c);
        if (g.wait_seconds >= 60)
            snprintf(c->status, sizeof(c->status), "Waiting for a free Xbox (about %u min)...",
                     (g.wait_seconds + 30) / 60);
        else
            snprintf(c->status, sizeof(c->status), "Waiting for a free Xbox...");
    } else if (g.provisioning_at && now - g.provisioning_at > XC_PROVISION_LIMIT_MS) {
        /* Seen as "Provisioning" or "ReadyToConnect" for minutes. */
        session_failed(c, "The Xbox didn't finish starting. Try again in a moment.", "slow-start");
    } else if (!strcmp(state, "Provisioning")) {
        c->session_state = GFN_SESSION_SETUP;
        if (!g.provisioning_at) g.provisioning_at = now;
        snprintf(c->status, sizeof(c->status), "Starting the Xbox...");
    } else if (!strcmp(state, "ReadyToConnect")) {
        if (!g.provisioning_at) g.provisioning_at = now;
        if (!g.connect_sent) {
            snprintf(c->status, sizeof(c->status), "Connecting your account to the Xbox...");
            g.connect_sent = connect_session(c);
            if (!g.connect_sent) session_failed(c, "The Xbox didn't accept the sign-in. Try again.", "connect");
        }
    } else if (!strcmp(state, "Provisioned")) {
        c->session_state = GFN_SESSION_READY;
        g.next_keepalive_at = now + 30000;
        snprintf(c->status, sizeof(c->status), "Xbox ready; connecting the stream...");
    } else if (!strcmp(state, "Failed")) {
        char why[120];
        error_text(&r, why, sizeof(why));
        char text[160];
        snprintf(text, sizeof(text), "Xbox Cloud Gaming couldn't start the game%s%.110s", why[0] ? ": " : ".", why);
        session_failed(c, text, "failed");
    }
    json_decref(root);
    http_response_free(&r);
}

bool xcloud_recover_session(GfnClient *c)
{
    if (!c->session_id[0]) return false;
    char path[200];
    snprintf(path, sizeof(path), "/%s/state", c->session_id);
    HttpResponse r;
    if (!gs_request(c, "GET", path, NULL, &r)) return false;
    json_t *root = response_json(&r);
    const char *state = json_string_value(json_object_get(root, "state"));
    const bool alive = r.status == 200 && state && !strcmp(state, "Provisioned");
    diagnostic_log("XCLOUD", "recover: http=%ld state=%s", r.status, state ? state : "-");
    json_decref(root);
    http_response_free(&r);
    if (alive) {
        c->session_state = GFN_SESSION_READY;
        return true;
    }
    session_failed(c, "The Xbox session ended. Start the game again.", "ended");
    return false;
}

bool xcloud_stop_session(GfnClient *c)
{
    if (!c->session_id[0]) {
        c->session_state = GFN_SESSION_IDLE;
        return true;
    }
    char path[200];
    snprintf(path, sizeof(path), "/%s", c->session_id);
    HttpResponse r;
    if (!gs_request(c, "DELETE", path, NULL, &r)) {
        /* Unheard, the service ends the session itself within minutes (no
         * keepalive): nothing is left that needs this console. */
        diagnostic_log("XCLOUD", "stop session unsent; left to expire");
        memset(c->session_id, 0, sizeof(c->session_id));
        c->session_state = GFN_SESSION_IDLE;
        return true;
    }
    const long status = r.status;
    http_response_free(&r);
    diagnostic_log("XCLOUD", "stop session http=%ld", status);
    const bool ok = (status >= 200 && status < 300) || status == 404;
    if (ok) {
        memset(c->session_id, 0, sizeof(c->session_id));
        c->session_state = GFN_SESSION_IDLE;
        snprintf(c->status, sizeof(c->status), "Xbox session ended");
    }
    return ok;
}

/* ---- Offer, answer and ICE over HTTPS -------------------------------------------- */

/* GET until the service has the answer (204 while it works on it). */
static json_t *poll_exchange(GfnClient *c, const char *path, unsigned timeout_ms)
{
    const u64 deadline = osGetTime() + timeout_ms;
    while (osGetTime() < deadline) {
        HttpResponse r;
        if (!gs_request(c, "GET", path, NULL, &r)) return NULL;
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
                snprintf(why, sizeof(why), "%s", message ? message : "no answer");
                snprintf(c->status, sizeof(c->status), "Xbox stream setup failed: %.100s", why);
            }
            json_decref(root);
            return exchange;
        }
        const long status = r.status;
        http_response_free(&r);
        if (status != 204) {
            snprintf(c->status, sizeof(c->status), "Xbox stream setup: HTTP %ld", status);
            return NULL;
        }
        svcSleepThread(400000000LL);
    }
    snprintf(c->status, sizeof(c->status), "Xbox stream setup timed out");
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

bool xcloud_signal_start(NvstSignal *s, const GfnClient *client)
{
    nvst_signal_close(s);
    s->xcloud = true;
    s->started_ms = osGetTime();
    s->state = NVST_SIGNAL_WAITING;
    snprintf(s->status, sizeof(s->status), "Setting up the Xbox stream...");
    /* gs_request reports into a client; keep the worker's copy untouched. */
    static GfnClient scratch;
    memcpy(scratch.access_token, client->access_token, sizeof(scratch.access_token));
    snprintf(scratch.session_id, sizeof(scratch.session_id), "%s", client->session_id);
    login_load();

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
    snprintf(path, sizeof(path), "/%s/sdp", scratch.session_id);
    HttpResponse r;
    const bool posted = text && gs_request(&scratch, "POST", path, text, &r);
    free(text);
    if (!posted) {
        free(offer);
        return signal_fail(s, peer, scratch.status[0] ? scratch.status : "Couldn't send the stream offer");
    }
    const long offer_status = r.status;
    http_response_free(&r);
    if (offer_status < 200 || offer_status >= 300) {
        free(offer);
        char why[64];
        snprintf(why, sizeof(why), "The Xbox refused the stream offer (HTTP %ld)", offer_status);
        return signal_fail(s, peer, why);
    }
    json_t *answer = poll_exchange(&scratch, path, 20000);
    const char *answer_sdp = json_string_value(json_object_get(answer, "sdp"));
    const char *answer_status = json_string_value(json_object_get(answer, "status"));
    if (!answer_sdp || (answer_status && strcmp(answer_status, "success"))) {
        diagnostic_log("XCLOUD", "answer status=%s", answer_status ? answer_status : "-");
        json_decref(answer);
        free(offer);
        return signal_fail(s, peer, scratch.status[0] ? scratch.status : "The Xbox sent no stream answer");
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
    snprintf(path, sizeof(path), "/%s/ice", scratch.session_id);
    const bool sent = text && gs_request(&scratch, "POST", path, text, &r);
    free(text);
    if (!sent) return signal_fail(s, peer, "Couldn't send the network candidates");
    http_response_free(&r);
    json_t *remote = poll_exchange(&scratch, path, 15000);
    size_t index;
    json_t *item;
    json_array_foreach(remote, index, item) {
        const char *candidate = json_string_value(json_object_get(item, "candidate"));
        if (!candidate || strstr(candidate, "end-of-candidates") || s->remote_ice_count >= ARRAY_SIZE(s->remote_ice))
            continue;
        /* IPv4 only: the console has no IPv6. */
        char address[64] = "";
        sscanf(candidate, "a=candidate:%*s %*u %*s %*u %63s", address);
        if (strchr(address, ':')) continue;
        json_t *entry = json_pack("{s:s}", "candidate", candidate);
        char *line = entry ? json_dumps(entry, JSON_COMPACT) : NULL;
        json_decref(entry);
        if (line && strlen(line) < sizeof(s->remote_ice[0]))
            snprintf(s->remote_ice[s->remote_ice_count++], sizeof(s->remote_ice[0]), "%s", line);
        free(line);
    }
    json_decref(remote);
    diagnostic_log("XCLOUD", "remote candidates %u (IPv4)", s->remote_ice_count);
    if (!s->remote_ice_count || !s->offer_sdp) return signal_fail(s, peer, "The Xbox sent no usable address");
    s->xcloud_peer = peer;
    s->state = NVST_SIGNAL_OFFER;
    snprintf(s->status, sizeof(s->status), "Connecting to the Xbox...");
    return true;
}
