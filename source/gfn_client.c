#include "gfn_client.h"
#include "app_paths.h"
#include "http_client.h"
#include "diagnostic.h"
#include "stream_profile.h"
#include "regions.h"
#include "xcloud.h"
#include "steam_link.h"

#include <3ds.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define DATA_DIR APP_DATA_DIR
static void active_clear(void);
#define SESSION_PATH DATA_DIR "/gfn-session.json"
#define SESSION_TMP DATA_DIR "/gfn-session.tmp"
#define DEVICE_PATH DATA_DIR "/device-id2.txt" /* see SESSION_DEVICE_PATH */
#define ACTIVE_SESSION_PATH APP_DATA_DIR "/active-session.json"

static const char *DEVICE_CLIENT_ID = "q61ddeJrVt7O90Nl-P-N7I36yctih4Ml6FyXLrb6j-U";
static const char *DEVICE_UA = "Mozilla/5.0 (X11; Linux x86_64; Steam Deck) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36";
static const char *GFN_UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36 NVIDIACEFClient/HEAD/debb5919f6 GFN-PC/2.0.87.131";
/* CloudMatch sessions belong to the client id that made them. Beta.18 made a
 * new random one per launch, so a later launch could neither list nor DELETE
 * an earlier one (every stop: 404). OpenNOW desktop sends this fixed id. */
static const char *CLOUDMATCH_CLIENT_ID = "ec7e38d4-03af-4b58-b131-cfb0495903ab";
/* Requests are serialized on the app thread; avoid an 8 KiB caller frame during TLS. */
static char g_authorization_header[8300];

static bool cloudmatch_status_is_transient(long status)
{
    return status == 408 || status == 429 || status == 502 || status == 503 || status == 504;
}

/* Random bytes from the 3DS's hardware generator. rand() was not enough:
 * newlib keeps its state per thread, every thread starts at the same seed,
 * and the ids are made on the network worker, which was never seeded. So
 * every console made the same "random" device id, and NVIDIA counted all
 * Kasumi players' sessions against one device:
 * SESSION_LIMIT_PER_DEVICE_EXCEEDED with no session of the player's own,
 * worst at busy hours, gone when someone else's game ended. */
static void random_fill(unsigned char *bytes, size_t size)
{
    if (R_SUCCEEDED(psInit())) {
        const Result rc = PS_GenerateRandomBytes(bytes, size);
        psExit();
        if (R_SUCCEEDED(rc)) return;
    }
    diagnostic_log("APP", "hardware random unavailable; using the clock");
    for (size_t i = 0; i < size; ++i) {
        const u64 tick = svcGetSystemTick();
        bytes[i] = (unsigned char)(tick ^ (tick >> 8) ^ (tick >> 16) ^ (osGetTime() * 2654435761u));
        svcSleepThread(1000 + (tick & 0x3ff));
    }
}

static void generate_uuid(char output[40])
{
    unsigned char bytes[16];
    random_fill(bytes, sizeof(bytes));
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    snprintf(output, 40,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

static void copy_json_string(char *destination, size_t size, json_t *object, const char *key)
{
    json_t *value = json_object_get(object, key);
    const char *text = json_is_string(value) ? json_string_value(value) : "";
    snprintf(destination, size, "%s", text);
}

static bool copy_json_string_if_present(char *destination, size_t size,
                                        json_t *object, const char *key)
{
    json_t *value = json_object_get(object, key);
    if (!json_is_string(value)) return false;
    snprintf(destination, size, "%s", json_string_value(value));
    return true;
}

static bool save_session(const GfnClient *client)
{
    mkdir("sdmc:/3ds", 0777);
    mkdir(DATA_DIR, 0777);
    json_t *root = json_object();
    if (!root) return false;
    json_object_set_new(root, "access_token", json_string(client->access_token));
    json_object_set_new(root, "refresh_token", json_string(client->refresh_token));
    json_object_set_new(root, "id_token", json_string(client->id_token));
    json_object_set_new(root, "expires_at", json_integer(client->token_expires_at));
    json_object_set_new(root, "client_token", json_string(client->client_token));
    json_object_set_new(root, "client_token_expires_at", json_integer(client->client_token_expires_at));
    json_object_set_new(root, "user_id", json_string(client->user_id));
    GfnProvider provider;
    provider_active(&provider);
    json_object_set_new(root, "provider", json_pack("{s:s,s:s,s:s,s:s}", "code", provider.code,
                                                    "name", provider.name, "idp", provider.idp, "url", provider.url));
    const int result = json_dump_file(root, SESSION_TMP, JSON_COMPACT);
    json_decref(root);
    if (result != 0) return false;
    remove(SESSION_PATH);
    return rename(SESSION_TMP, SESSION_PATH) == 0;
}

static bool load_session(GfnClient *client)
{
    json_error_t error;
    json_t *root = json_load_file(SESSION_PATH, 0, &error);
    if (!root) return false;
    copy_json_string(client->access_token, sizeof(client->access_token), root, "access_token");
    copy_json_string(client->refresh_token, sizeof(client->refresh_token), root, "refresh_token");
    copy_json_string(client->id_token, sizeof(client->id_token), root, "id_token");
    json_t *expires = json_object_get(root, "expires_at");
    client->token_expires_at = json_is_integer(expires) ? json_integer_value(expires) : 0;
    copy_json_string(client->client_token, sizeof(client->client_token), root, "client_token");
    json_t *client_expires = json_object_get(root, "client_token_expires_at");
    client->client_token_expires_at = json_is_integer(client_expires) ? json_integer_value(client_expires) : 0;
    copy_json_string(client->user_id, sizeof(client->user_id), root, "user_id");
    /* Logins saved before providers existed are NVIDIA's. */
    GfnProvider provider;
    provider_nvidia(&provider);
    json_t *saved = json_object_get(root, "provider");
    if (json_is_object(saved)) {
        copy_json_string(provider.code, sizeof(provider.code), saved, "code");
        copy_json_string(provider.name, sizeof(provider.name), saved, "name");
        copy_json_string(provider.idp, sizeof(provider.idp), saved, "idp");
        copy_json_string(provider.url, sizeof(provider.url), saved, "url");
    }
    provider_set_active(&provider);
    json_decref(root);
    return client->access_token[0] != '\0';
}

static void get_device_id(char output[40])
{
    FILE *file = fopen(DEVICE_PATH, "r");
    if (file) {
        if (fgets(output, 40, file) && strlen(output) >= 32) {
            output[strcspn(output, "\r\n")] = '\0';
            fclose(file);
            return;
        }
        fclose(file);
    }
    mkdir("sdmc:/3ds", 0777);
    mkdir(DATA_DIR, 0777);
    generate_uuid(output);
    diagnostic_log("AUTH", "new device id made (one per console)");
    file = fopen(DEVICE_PATH, "w");
    if (file) { fputs(output, file); fclose(file); }
}

/* The device id game sessions use (x-device-id, deviceHashId). It starts as
 * the sign-in device id and is replaced when NVIDIA refuses every launch
 * "per device" while no session exists. The sign-in keeps its own id, so the
 * saved login is not affected. Both files are new in beta.26: ids made by
 * beta.25 and older were shared by every console (see random_fill). */
#define SESSION_DEVICE_PATH DATA_DIR "/session-device-id2.txt"

static void get_session_device_id(char output[40])
{
    FILE *file = fopen(SESSION_DEVICE_PATH, "r");
    if (file) {
        const bool ok = fgets(output, 40, file) && strlen(output) >= 32;
        fclose(file);
        if (ok) {
            output[strcspn(output, "\r\n")] = '\0';
            return;
        }
    }
    get_device_id(output);
}

static void new_session_device_id(char output[40])
{
    generate_uuid(output);
    FILE *file = fopen(SESSION_DEVICE_PATH, "w");
    if (file) { fputs(output, file); fclose(file); }
}

static json_t *parse_response(HttpResponse *response, GfnClient *client, const char *operation)
{
    if (response->status < 200 || response->status >= 300) {
        snprintf(client->status, sizeof(client->status), "%s: HTTP %ld", operation, response->status);
        return NULL;
    }
    json_error_t error;
    json_t *json = json_loadb(response->body ? response->body : "", response->size, 0, &error);
    if (!json)
        snprintf(client->status, sizeof(client->status), "%s: invalid JSON line %d", operation, error.line);
    return json;
}

static bool fetch_user_id(GfnClient *client)
{
    if (client->user_id[0]) return true;
    snprintf(g_authorization_header, sizeof(g_authorization_header),
             "Authorization: Bearer %s", client->access_token);
    const char *headers[] = {
        "Origin: https://nvfile", "Referer: https://nvfile/",
        "Accept: application/json", g_authorization_header
    };
    HttpResponse response;
    if (!http_request("GET", "https://login.nvidia.com/userinfo", DEVICE_UA,
                      headers, ARRAY_SIZE(headers), NULL, 128 * 1024, &response))
        return false;
    json_error_t error;
    json_t *root = json_loadb(response.body ? response.body : "", response.size, 0, &error);
    if (response.status == 200 && root)
        copy_json_string(client->user_id, sizeof(client->user_id), root, "sub");
    diagnostic_log("AUTH", "userinfo http=%ld userId=%s", response.status,
                   client->user_id[0] ? "present" : "missing");
    if (root) json_decref(root);
    http_response_free(&response);
    return client->user_id[0] != '\0';
}

static bool fetch_client_token(GfnClient *client)
{
    const int64_t now = (int64_t)time(NULL);
    if (client->client_token[0] && now + 600 < client->client_token_expires_at)
        return true;
    snprintf(g_authorization_header, sizeof(g_authorization_header),
             "Authorization: Bearer %s", client->access_token);
    const char *headers[] = {
        "Origin: https://nvfile", "Referer: https://nvfile/",
        "Accept: application/json, text/plain, */*", g_authorization_header
    };
    HttpResponse response;
    if (!http_request("GET", "https://login.nvidia.com/client_token", DEVICE_UA,
                      headers, ARRAY_SIZE(headers), NULL, 128 * 1024, &response))
        return false;
    json_error_t error;
    json_t *root = json_loadb(response.body ? response.body : "", response.size, 0, &error);
    if (response.status == 200 && root) {
        copy_json_string_if_present(client->client_token, sizeof(client->client_token), root, "client_token");
        json_t *expires = json_object_get(root, "expires_in");
        client->client_token_expires_at = now +
            (json_is_integer(expires) ? json_integer_value(expires) : 86400);
    }
    diagnostic_log("AUTH", "client-token http=%ld token=%s lasts=%llds", response.status,
                   client->client_token[0] ? "present" : "missing",
                   (long long)(client->client_token_expires_at - now));
    if (root) json_decref(root);
    http_response_free(&response);
    return client->client_token[0] != '\0';
}

static void hydrate_session_identity(GfnClient *client)
{
    if (!client->access_token[0]) return;
    fetch_user_id(client);
    fetch_client_token(client);
}

/* The last token request failed for a passing reason (no answer, 408, 429,
 * 5xx) rather than NVIDIA rejecting the credential. As in the other OpenNOW
 * clients, only a rejection may end a saved login. */
static bool g_token_transient;

static bool token_status_transient(long status)
{
    return status == 408 || status == 429 || status >= 500;
}

static bool request_tokens(GfnClient *client, const char *form, bool polling)
{
    const char *headers[] = {
        "Origin: https://play.geforcenow.com",
        "Referer: https://play.geforcenow.com/",
        "Accept: application/json, text/plain, */*",
        "Content-Type: application/x-www-form-urlencoded; charset=UTF-8"
    };
    HttpResponse response;
    g_token_transient = false;
    /* A refresh tries three times over two seconds; the sign-in poll has its
     * own interval. */
    const int attempts = polling ? 1 : 3;
    bool sent = false;
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        sent = http_request("POST", "https://login.nvidia.com/token", DEVICE_UA,
                            headers, ARRAY_SIZE(headers), form, 128 * 1024, &response);
        if ((sent && !token_status_transient(response.status)) || attempt == attempts) break;
        if (sent) http_response_free(&response);
        svcSleepThread(attempt == 1 ? 500000000LL : 1500000000LL);
    }
    if (!sent) {
        g_token_transient = true;
        snprintf(client->status, sizeof(client->status), "Token request: %.130s", response.error);
        diagnostic_log("AUTH", "token request got no answer: %.100s", response.error);
        return false;
    }
    json_error_t error;
    json_t *root = json_loadb(response.body ? response.body : "", response.size, 0, &error);
    if (response.status == 200 && root) {
        json_t *access = json_object_get(root, "access_token");
        if (!json_is_string(access) || !json_string_length(access)) {
            snprintf(client->status, sizeof(client->status), "Login response has no access token; X to sign in again");
            client->auth_state = GFN_AUTH_ERROR;
            json_decref(root);
            http_response_free(&response);
            return false;
        }
        copy_json_string(client->access_token, sizeof(client->access_token), root, "access_token");
        copy_json_string_if_present(client->refresh_token, sizeof(client->refresh_token), root, "refresh_token");
        copy_json_string_if_present(client->id_token, sizeof(client->id_token), root, "id_token");
        const bool rotated_client_token = copy_json_string_if_present(
            client->client_token, sizeof(client->client_token), root, "client_token");
        json_t *expires = json_object_get(root, "expires_in");
        const int64_t now = (int64_t)time(NULL);
        const int64_t lifetime =
            (json_is_integer(expires) ? json_integer_value(expires) : 86400);
        client->token_expires_at = now + lifetime;
        /* A client token handed back here gets its real lifetime from
         * /client_token below (as OpenNOW Vita). Beta.23 gave it the access
         * token's hour, so it was never renewed in time and NVIDIA refused
         * the next renewal with 401: twelve sign-outs in a day. */
        if (rotated_client_token) client->client_token_expires_at = 0;
        diagnostic_log("AUTH", "tokens renewed: login lasts %llds, client token %s",
                       (long long)lifetime, rotated_client_token ? "replaced" : "kept");
        json_decref(root);
        http_response_free(&response);
        client->auth_state = GFN_AUTH_LOGGED_IN;
        if (polling) {
            provider_set_active(&client->login_provider);
            diagnostic_log("AUTH", "signed in through provider %s", client->login_provider.code);
        }
        hydrate_session_identity(client);
        const bool saved = save_session(client);
        snprintf(client->status, sizeof(client->status), "Signed in; session %s", saved ? "saved to SD" : "save failed");
        return true;
    }

    const char *code = "", *description = "";
    if (root) {
        json_t *error_value = json_object_get(root, "error");
        if (json_is_string(error_value)) code = json_string_value(error_value);
        json_t *description_value = json_object_get(root, "error_description");
        if (json_is_string(description_value)) description = json_string_value(description_value);
    }
    if (polling && strcmp(code, "authorization_pending") == 0) {
        snprintf(client->status, sizeof(client->status), "Waiting for browser sign-in...");
    } else if (polling && strcmp(code, "slow_down") == 0) {
        client->poll_interval += 5;
        snprintf(client->status, sizeof(client->status), "NVIDIA asked to slow polling");
    } else {
        /* Beta.22 report YVQ446: a partner login's refresh was refused and
         * the log couldn't say why. */
        diagnostic_log("AUTH", "token request failed: http=%ld error=%.40s (%.80s)",
                       response.status, code, description);
        g_token_transient = token_status_transient(response.status);
        if (g_token_transient) {
            snprintf(client->status, sizeof(client->status),
                     "NVIDIA's login server is busy (HTTP %ld). Try again in a moment.", response.status);
        } else {
            snprintf(client->status, sizeof(client->status), "Token HTTP %ld: %.80s", response.status, code);
            /* A refresh decides for itself (refresh_failed). */
            if (polling) client->auth_state = GFN_AUTH_ERROR;
        }
    }
    if (root) json_decref(root);
    http_response_free(&response);
    return false;
}

/* A refresh that didn't work. The login stays while it lasts, and a passing
 * failure (no Wi-Fi yet, a busy server) never signs anyone out: beta.18
 * report J8E-HWH (Wi-Fi not back yet) and beta.22 report YVQ446 (refused,
 * partner login) both sent players back to the sign-in code. */
static bool refresh_failed(GfnClient *client, bool transient)
{
    const int64_t left = client->token_expires_at - (int64_t)time(NULL);
    if (left > 60) {
        diagnostic_log("AUTH", "refresh failed (%s); the current login lasts %lld s more",
                       transient ? "temporary" : "refused", (long long)left);
        return true;
    }
    if (transient) {
        snprintf(client->status, sizeof(client->status),
                 "Can't reach NVIDIA's login server right now. Check the Wi-Fi, then try again.");
        return false;
    }
    diagnostic_log("AUTH", "refresh refused and the login has run out: sign-in needed");
    client->auth_state = GFN_AUTH_ERROR;
    snprintf(client->status, sizeof(client->status), "Your NVIDIA login has expired. Press X to sign in again.");
    return false;
}

static bool refresh_session(GfnClient *client)
{
    if ((int64_t)time(NULL) + 600 < client->token_expires_at) {
        if (!client->user_id[0] || !client->client_token[0] ||
            (int64_t)time(NULL) + 600 >= client->client_token_expires_at) {
            hydrate_session_identity(client);
            save_session(client);
        }
        return true;
    }
    bool transient = false;
    if (client->client_token[0] && client->user_id[0]) {
        bool tried = false;
        char *encoded_token = http_url_encode(client->client_token);
        char *encoded_user = http_url_encode(client->user_id);
        if (encoded_token && encoded_user) {
            const size_t length = strlen(encoded_token) + strlen(encoded_user) +
                                  strlen(DEVICE_CLIENT_ID) + 192;
            char *form = malloc(length);
            if (form) {
                snprintf(form, length,
                         "grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Aclient_token&client_token=%s&client_id=%s&sub=%s",
                         encoded_token, DEVICE_CLIENT_ID, encoded_user);
                diagnostic_log("AUTH", "refresh method=client-token");
                const bool ok = request_tokens(client, form, false);
                tried = true;
                free(form);
                free(encoded_token);
                free(encoded_user);
                if (ok) return true;
            } else {
                free(encoded_token);
                free(encoded_user);
            }
        } else {
            free(encoded_token);
            free(encoded_user);
        }
        transient = !tried || g_token_transient;
        diagnostic_log("AUTH", "client-token refresh failed (%s)%s", transient ? "temporary" : "refused",
                       client->refresh_token[0] ? "; trying OAuth refresh" : "");
    }
    if (!client->refresh_token[0]) return refresh_failed(client, transient);
    char *encoded = http_url_encode(client->refresh_token);
    if (!encoded) return refresh_failed(client, true);
    const size_t length = strlen(encoded) + strlen(DEVICE_CLIENT_ID) + 96;
    char *form = malloc(length);
    if (!form) {
        free(encoded);
        return refresh_failed(client, true);
    }
    snprintf(form, length, "grant_type=refresh_token&refresh_token=%s&client_id=%s", encoded, DEVICE_CLIENT_ID);
    free(encoded);
    diagnostic_log("AUTH", "refresh method=oauth-refresh-token");
    const bool ok = request_tokens(client, form, false);
    free(form);
    return ok || refresh_failed(client, transient || g_token_transient);
}

void gfn_client_init(GfnClient *client)
{
    memset(client, 0, sizeof(*client));
    srand((unsigned)(svcGetSystemTick() ^ osGetTime()));
    /* The NVIDIA login sets its provider (partners) even while Xbox is the
     * service shown; then its tokens make way for Xbox's. */
    const bool nvidia_saved = load_session(client);
    if (xcloud_selected() || steam_link_selected()) {
        memset(client->access_token, 0, sizeof(client->access_token));
        memset(client->refresh_token, 0, sizeof(client->refresh_token));
        memset(client->id_token, 0, sizeof(client->id_token));
        memset(client->client_token, 0, sizeof(client->client_token));
        client->user_id[0] = '\0';
        client->token_expires_at = client->client_token_expires_at = 0;
        if (steam_link_selected()) {
            if (steam_link_load_login(client)) {
                client->auth_state = GFN_AUTH_LOGGED_IN;
                snprintf(client->status, sizeof(client->status), "Paired with %.60s", steam_link_host_name());
            } else {
                client->auth_state = GFN_AUTH_LOGGED_OUT;
                snprintf(client->status, sizeof(client->status), "Press X to pair with your PC");
            }
            return;
        }
        if (xcloud_load_login(client)) {
            diagnostic_log("AUTH", "saved Xbox login%s", nvidia_saved ? " (NVIDIA login saved too)" : "");
            client->auth_state = GFN_AUTH_LOGGED_IN;
            snprintf(client->status, sizeof(client->status), "Saved Xbox login loaded");
        } else {
            client->auth_state = GFN_AUTH_LOGGED_OUT;
            snprintf(client->status, sizeof(client->status), "Press X to sign in with Microsoft");
        }
        return;
    }
    if (nvidia_saved) {
        GfnProvider provider;
        provider_active(&provider);
        diagnostic_log("AUTH", "saved login, provider %s", provider.code);
        client->auth_state = GFN_AUTH_LOGGED_IN;
        snprintf(client->status, sizeof(client->status), "Saved NVIDIA session loaded");
    } else {
        client->auth_state = GFN_AUTH_LOGGED_OUT;
        snprintf(client->status, sizeof(client->status), "Press X to sign in with NVIDIA");
    }
}

bool gfn_begin_login(GfnClient *client, const char *provider_choice)
{
    client->catalog_vpc[0] = '\0';
    if (steam_link_selected()) {
        diagnostic_log("AUTH", "pairing with a PC (Steam Link)");
        return steam_link_begin_login(client);
    }
    if (xcloud_selected()) {
        diagnostic_log("AUTH", "sign-in through Xbox Cloud Gaming");
        return xcloud_begin_login(client);
    }
    /* Refresh the provider list (quick, no login needed); the cached one
     * serves when it can't be reached. */
    providers_fetch();
    GfnProvider provider;
    if (provider_choice && provider_choice[0]) {
        if (!providers_find(provider_choice, &provider)) {
            diagnostic_log("AUTH", "provider %s not listed; using NVIDIA", provider_choice);
            provider_nvidia(&provider);
        }
    } else {
        /* Default: NVIDIA, as OpenNOW desktop. Beta.21 followed NVIDIA's
         * recommendation for the country (as OpenNOW Vita): a player in Chile
         * with an NVIDIA account was signed in through Digevo, got an empty
         * library and ENTITLEMENT_FAILURE on every launch. Partner accounts
         * pick their partner in Settings > Account. */
        provider_nvidia(&provider);
    }
    client->login_provider = provider;
    GfnProvider recommended;
    providers_recommended(&recommended);
    diagnostic_log("AUTH", "sign-in through provider %s (%s; recommended here %s)", provider.code,
                   provider_choice && provider_choice[0] ? "chosen" : "default", recommended.code);
    char device_id[40];
    get_device_id(device_id);
    char form[768];
    snprintf(form, sizeof(form),
             "client_id=%s&scope=openid%%20consent%%20email%%20tk_client%%20age&device_id=%s&display_name=Kasumi-3DS&idp_id=%s",
             DEVICE_CLIENT_ID, device_id, provider.idp);
    char device_header[80];
    snprintf(device_header, sizeof(device_header), "x-device-id: %s", device_id);
    const char *headers[] = {
        "Origin: https://play.geforcenow.com", "Referer: https://play.geforcenow.com/",
        "Accept: application/json, text/plain, */*",
        "Content-Type: application/x-www-form-urlencoded; charset=UTF-8", device_header,
        "nv-client-id: q61ddeJrVt7O90Nl-P-N7I36yctih4Ml6FyXLrb6j-U",
        "nv-client-streamer: WEBRTC", "nv-client-type: BROWSER",
        "nv-client-platform-name: browser", "nv-browser-type: CHROME",
        "nv-device-os: STEAMOS", "nv-device-type: CONSOLE",
        "nv-device-model: STEAMDECK", "nv-device-make: VALVE"
    };
    snprintf(client->status, sizeof(client->status), "Contacting NVIDIA login...");
    HttpResponse response;
    if (!http_request("POST", "https://login.nvidia.com/device/authorize", DEVICE_UA,
                      headers, ARRAY_SIZE(headers), form, 128 * 1024, &response)) {
        char date[16];
        if (http_clock_wrong(date, sizeof(date)))
            snprintf(client->status, sizeof(client->status),
                     "Your 3DS thinks it is %s, so NVIDIA's secure sign-in fails. Set the date and time in "
                     "System Settings > Other Settings, then try again.", date);
        else
            snprintf(client->status, sizeof(client->status),
                     "Couldn't reach NVIDIA's sign-in (%.60s). Check Wi-Fi, and that the 3DS date and time "
                     "are right.", response.error);
        client->auth_state = GFN_AUTH_ERROR;
        return false;
    }
    json_t *root = parse_response(&response, client, "Device login");
    if (!root) {
        http_response_free(&response);
        client->auth_state = GFN_AUTH_ERROR;
        return false;
    }
    copy_json_string(client->device_code, sizeof(client->device_code), root, "device_code");
    copy_json_string(client->user_code, sizeof(client->user_code), root, "user_code");
    copy_json_string(client->verification_uri, sizeof(client->verification_uri), root, "verification_uri");
    json_t *interval = json_object_get(root, "interval");
    json_t *expires = json_object_get(root, "expires_in");
    client->poll_interval = json_is_integer(interval) ? (int)json_integer_value(interval) : 5;
    if (client->poll_interval < 1) client->poll_interval = 1;
    const int64_t now = (int64_t)time(NULL);
    client->next_poll_at = now + client->poll_interval;
    client->challenge_expires_at = now + (json_is_integer(expires) ? json_integer_value(expires) : 300);
    json_decref(root);
    http_response_free(&response);
    if (!client->device_code[0] || !client->user_code[0] || !client->verification_uri[0]) {
        snprintf(client->status, sizeof(client->status), "NVIDIA login response incomplete");
        client->auth_state = GFN_AUTH_ERROR;
        return false;
    }
    client->auth_state = GFN_AUTH_WAITING;
    if (strcmp(provider.code, PROVIDER_NVIDIA))
        snprintf(client->status, sizeof(client->status), "Signing in through %s: open URL on phone/PC and enter code",
                 provider.name);
    else
        snprintf(client->status, sizeof(client->status), "Open URL on phone/PC and enter code");
    return true;
}

void gfn_tick(GfnClient *client)
{
    /* Also when not waiting: a cancelled pairing is closed there. */
    if (steam_link_selected()) {
        steam_link_login_tick(client);
        return;
    }
    if (client->auth_state != GFN_AUTH_WAITING) return;
    if (xcloud_selected()) {
        xcloud_login_tick(client);
        return;
    }
    const int64_t now = (int64_t)time(NULL);
    if (now >= client->challenge_expires_at) {
        client->auth_state = GFN_AUTH_ERROR;
        snprintf(client->status, sizeof(client->status), "Login code expired; press X to retry");
        return;
    }
    if (now < client->next_poll_at) return;
    client->next_poll_at = now + client->poll_interval;
    char *encoded = http_url_encode(client->device_code);
    if (!encoded) return;
    const size_t length = strlen(encoded) + strlen(DEVICE_CLIENT_ID) + 128;
    char *form = malloc(length);
    if (!form) { free(encoded); return; }
    snprintf(form, length,
             "grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code&device_code=%s&client_id=%s",
             encoded, DEVICE_CLIENT_ID);
    free(encoded);
    request_tokens(client, form, true);
    free(form);
}

bool gfn_has_session(const GfnClient *client)
{
    return client->auth_state == GFN_AUTH_LOGGED_IN && client->access_token[0];
}

const char *gfn_bearer_token(const GfnClient *client)
{
    return client->id_token[0] ? client->id_token : client->access_token;
}

static bool fetch_catalog(GfnClient *client, const char *search_query, bool owned_only)
{
    client->game_count = 0;
    client->catalog_total = 0;
    if (!gfn_has_session(client)) {
        snprintf(client->status, sizeof(client->status), "Sign in before loading games");
        return false;
    }
    if (!refresh_session(client)) {
        return false;
    }

    const char *token = gfn_bearer_token(client);
    snprintf(g_authorization_header, sizeof(g_authorization_header),
             "Authorization: GFNJWT %s", token);
    const char *lcars_headers[] = {
        "Accept: application/json", g_authorization_header,
        "nv-client-id: ec7e38d4-03af-4b58-b131-cfb0495903ab",
        "nv-client-type: BROWSER", "nv-client-version: 2.0.80.173",
        "nv-client-streamer: WEBRTC", "nv-device-os: WINDOWS", "nv-device-type: DESKTOP"
    };
    char vpc_id[64] = "GFN-PC";
    /* The library's VPC comes from the account's own provider (a partner's
     * serverInfo names its own), as in OpenNOW desktop. */
    char base[96], info_url[128];
    provider_base_url(base, sizeof(base));
    snprintf(info_url, sizeof(info_url), "%s/v2/serverInfo", base);
    HttpResponse server_info;
    if (client->catalog_vpc[0] && (int64_t)time(NULL) < client->catalog_vpc_expires_at) {
        snprintf(vpc_id, sizeof(vpc_id), "%s", client->catalog_vpc);
    } else if (http_request("GET", info_url, GFN_UA,
                     lcars_headers, ARRAY_SIZE(lcars_headers), NULL, 256 * 1024, &server_info)) {
        if (server_info.status == 401 || server_info.status == 403) {
            snprintf(client->status, sizeof(client->status), "GFN rejected saved login (HTTP %ld)", server_info.status);
            client->auth_state = GFN_AUTH_ERROR;
            http_response_free(&server_info);
            return false;
        }
        json_error_t error;
        json_t *root = json_loadb(server_info.body ? server_info.body : "", server_info.size, 0, &error);
        if (root) {
            json_t *request_status = json_object_get(root, "requestStatus");
            if (json_is_object(request_status)) copy_json_string(vpc_id, sizeof(vpc_id), request_status, "serverId");
            if (vpc_id[0] && server_info.status == 200) {
                snprintf(client->catalog_vpc, sizeof(client->catalog_vpc), "%s", vpc_id);
                client->catalog_vpc_expires_at = (int64_t)time(NULL) + 600;
            }
            json_decref(root);
        }
        http_response_free(&server_info);
    }
    if (!vpc_id[0]) snprintf(vpc_id, sizeof(vpc_id), "GFN-PC");

    /* The library query mirrors OpenNOW's: each variant's library status is
     * requested and checked here too. Build 67 only relied on the server
     * filter and sorted by catalog relevance, so games that were never added
     * to the account showed up as "library". */
    #define APP_FIELDS "items{id title images{GAME_BOX_ART KEY_ART TV_BANNER HERO_IMAGE} " \
        "variants{id appStore gfn{library{status selected lastPlayedDate}}}}" \
        "pageInfo{hasNextPage endCursor totalCount}"
    static const char *browse_query =
        "query GetLibraryApps($vpcId:String!,$locale:String!,$sortString:String!,$fetchCount:Int!,$cursor:String!,$filters:AppFilterFields!){"
        "apps(vpcId:$vpcId,language:$locale,orderBy:$sortString,first:$fetchCount,after:$cursor,filters:$filters){"
        APP_FIELDS "}}";
    static const char *search_document =
        "query GetSearchCatalogApps($vpcId:String!,$locale:String!,$sortString:String!,$fetchCount:Int!,$cursor:String!,$searchString:String!,$filters:AppFilterFields!){"
        "apps(vpcId:$vpcId,language:$locale,orderBy:$sortString,first:$fetchCount,after:$cursor,searchQuery:$searchString,filters:$filters){"
        APP_FIELDS "}}";
    #undef APP_FIELDS
    char cursor[256] = "";
    unsigned returned = 0, skipped = 0;
    for (int page = 0; page < 8 && client->game_count < GFN_MAX_GAMES; ++page) {
    json_t *body_root = json_object();
    json_t *variables = json_object();
    json_t *filters = owned_only
        ? json_pack("{s:{s:{s:{s:{s:s}}}}}", "variants", "gfn", "library", "status", "notEquals", "NOT_OWNED")
        : json_object();
    json_object_set_new(body_root, "query", json_string(search_query ? search_document : browse_query));
    json_object_set_new(variables, "vpcId", json_string(vpc_id));
    json_object_set_new(variables, "locale", json_string("en_US"));
    json_object_set_new(variables, "sortString", json_string(owned_only
        ? "variants.gfn.library.lastPlayedDate:DESC,computedValues.libraryAddedDate:DESC,sortName:ASC"
        : "itemMetadata.relevance:DESC,sortName:ASC"));
    json_object_set_new(variables, "fetchCount", json_integer(owned_only ? 64 : 40));
    json_object_set_new(variables, "cursor", json_string(cursor));
    json_object_set_new(variables, "filters", filters);
    if (search_query) json_object_set_new(variables, "searchString", json_string(search_query));
    json_object_set_new(body_root, "variables", variables);
    char *body = json_dumps(body_root, JSON_COMPACT);
    json_decref(body_root);
    if (!body) return false;

    const char *graphql_headers[] = {
        "Accept: application/json, text/plain, */*", "Content-Type: application/json",
        "Origin: https://play.geforcenow.com", "Referer: https://play.geforcenow.com/", g_authorization_header,
        "nv-client-id: ec7e38d4-03af-4b58-b131-cfb0495903ab", "nv-client-type: NATIVE",
        "nv-client-version: 2.0.80.173", "nv-client-streamer: NVIDIA-CLASSIC",
        "nv-device-os: WINDOWS", "nv-device-type: DESKTOP", "nv-device-make: UNKNOWN",
        "nv-device-model: UNKNOWN", "nv-browser-type: CHROME"
    };
    HttpResponse response;
    const bool sent = http_request("POST", "https://games.geforce.com/graphql", GFN_UA,
                                   graphql_headers, ARRAY_SIZE(graphql_headers), body, 2 * 1024 * 1024, &response);
    free(body);
    if (!sent) {
        snprintf(client->status, sizeof(client->status), "Catalog network: %.130s", response.error);
        return false;
    }
    json_t *root = parse_response(&response, client, "Catalog");
    if (!root) { http_response_free(&response); return false; }
    json_t *errors = json_object_get(root, "errors");
    if (json_is_array(errors) && json_array_size(errors)) {
        json_t *first = json_array_get(errors, 0);
        json_t *message = json_is_object(first) ? json_object_get(first, "message") : NULL;
        snprintf(client->status, sizeof(client->status), "GFN: %.120s", json_is_string(message) ? json_string_value(message) : "GraphQL error");
        json_decref(root); http_response_free(&response); return false;
    }
    json_t *data = json_object_get(root, "data");
    json_t *apps = json_is_object(data) ? json_object_get(data, "apps") : NULL;
    json_t *items = json_is_object(apps) ? json_object_get(apps, "items") : NULL;
    json_t *page_info = json_is_object(apps) ? json_object_get(apps, "pageInfo") : NULL;
    json_t *total = json_is_object(page_info) ? json_object_get(page_info, "totalCount") : NULL;
    if (json_is_integer(total) && json_integer_value(total) >= 0) client->catalog_total = (size_t)json_integer_value(total);
    size_t index; json_t *item;
    json_array_foreach(items, index, item) {
        ++returned;
        if (client->game_count >= GFN_MAX_GAMES) break;
        json_t *title = json_object_get(item, "title");
        json_t *id = json_object_get(item, "id");
        if (!json_is_string(title) || !json_is_string(id)) continue;
        /* Pick the variant the account uses: the one selected in the
         * library, else any in the library, else the first launchable. */
        json_t *variants = json_object_get(item, "variants");
        json_t *chosen = NULL, *in_library = NULL, *launchable = NULL;
        size_t variant_index; json_t *variant;
        json_array_foreach(variants, variant_index, variant) {
            json_t *library = json_object_get(json_object_get(variant, "gfn"), "library");
            json_t *status = json_is_object(library) ? json_object_get(library, "status") : NULL;
            const char *s = json_is_string(status) ? json_string_value(status) : "";
            const bool owned = !strcmp(s, "MANUAL") || !strcmp(s, "PLATFORM_SYNC") || !strcmp(s, "IN_LIBRARY");
            if (owned && !in_library) in_library = variant;
            if (owned && json_is_true(json_object_get(library, "selected"))) chosen = variant;
            json_t *variant_id = json_object_get(variant, "id");
            const char *candidate = json_is_string(variant_id) ? json_string_value(variant_id) : "";
            bool numeric = candidate[0] != '\0';
            for (const char *p = candidate; *p; ++p) if (*p < '0' || *p > '9') numeric = false;
            if (numeric && !launchable) launchable = variant;
        }
        if (owned_only && !in_library) { ++skipped; continue; }
        if (!chosen) chosen = in_library ? in_library : launchable;
        GfnGame *game = &client->games[client->game_count];
        memset(game, 0, sizeof(*game));
        snprintf(game->title, sizeof(game->title), "%s", json_string_value(title));
        snprintf(game->app_id, sizeof(game->app_id), "%s", json_string_value(id));
        if (chosen) {
            json_t *variant_id = json_object_get(chosen, "id");
            if (json_is_string(variant_id))
                snprintf(game->app_id, sizeof(game->app_id), "%s", json_string_value(variant_id));
            json_t *store = json_object_get(chosen, "appStore");
            if (json_is_string(store)) snprintf(game->store, sizeof(game->store), "%s", json_string_value(store));
        }
        /* All launchable store versions, for the details page. */
        json_array_foreach(variants, variant_index, variant) {
            if (game->variant_count >= GFN_MAX_VARIANTS) break;
            json_t *variant_id = json_object_get(variant, "id");
            json_t *store = json_object_get(variant, "appStore");
            if (!json_is_string(variant_id)) continue;
            const char *vid = json_string_value(variant_id);
            bool numeric = vid[0] != '\0';
            for (const char *p = vid; *p; ++p) if (*p < '0' || *p > '9') numeric = false;
            if (!numeric || strlen(vid) >= sizeof(game->variants[0].id)) continue;
            unsigned n = game->variant_count++;
            snprintf(game->variants[n].id, sizeof(game->variants[n].id), "%s", vid);
            snprintf(game->variants[n].store, sizeof(game->variants[n].store), "%s",
                     json_is_string(store) ? json_string_value(store) : "GFN");
            if (!strcmp(vid, game->app_id)) game->variant_selected = n;
        }
        /* Box art first; it may be one URL or a list of them. */
        json_t *images = json_object_get(item, "images");
        static const char *const keys[] = { "GAME_BOX_ART", "KEY_ART", "TV_BANNER" };
        for (size_t k = 0; k < 3 && !game->image_url[0]; ++k) {
            json_t *image = json_is_object(images) ? json_object_get(images, keys[k]) : NULL;
            if (json_is_array(image)) image = json_array_get(image, 0);
            if (json_is_string(image) && json_string_value(image)[0])
                snprintf(game->image_url, sizeof(game->image_url), "%s", json_string_value(image));
        }
        /* Wide art for HOME Menu shortcut banners: the TV banner usually
         * carries the game's logo; key art and hero images may not. */
        static const char *const wide_keys[] = { "TV_BANNER", "KEY_ART", "HERO_IMAGE" };
        for (size_t k = 0; k < 3 && !game->wide_url[0]; ++k) {
            json_t *image = json_is_object(images) ? json_object_get(images, wide_keys[k]) : NULL;
            if (json_is_array(image)) image = json_array_get(image, 0);
            if (json_is_string(image) && json_string_value(image)[0])
                snprintf(game->wide_url, sizeof(game->wide_url), "%s", json_string_value(image));
        }
        ++client->game_count;
    }
    json_t *next = json_is_object(page_info) ? json_object_get(page_info, "hasNextPage") : NULL;
    json_t *end = json_is_object(page_info) ? json_object_get(page_info, "endCursor") : NULL;
    const bool more = json_is_true(next) && json_is_string(end) && json_string_value(end)[0] &&
                      strcmp(json_string_value(end), cursor) != 0;
    if (more) snprintf(cursor, sizeof(cursor), "%s", json_string_value(end));
    json_decref(root);
    http_response_free(&response);
    /* Search shows one page; the library pages until it is complete. */
    if (!more || search_query) break;
    }
    diagnostic_log("CATALOG", "%s returned=%u kept=%lu skippedNotInLibrary=%u total=%lu",
                   search_query ? "search" : "library", returned, (unsigned long)client->game_count,
                   skipped, (unsigned long)client->catalog_total);
    if (search_query) {
        snprintf(client->status, sizeof(client->status), "Search %.50s: %lu results",
                 search_query, (unsigned long)client->game_count);
    } else if (!client->game_count && !provider_is_nvidia()) {
        GfnProvider provider;
        provider_active(&provider);
        snprintf(client->status, sizeof(client->status),
                 "No games on %s for this account. NVIDIA account? Pick NVIDIA in Settings > Account and "
                 "sign in again.", provider.name);
        diagnostic_log("CATALOG", "empty library on partner %s", provider.code);
    } else {
        snprintf(client->status, sizeof(client->status), "Loaded %lu owned games (server total %lu)",
                 (unsigned long)client->game_count, (unsigned long)client->catalog_total);
    }
    return true;
}

#define LIBRARY_CACHE_PATH APP_DATA_DIR "/library.json"
/* Xbox's library is kept apart (xcloud.c deletes it on sign-out). */
#define XBOX_LIBRARY_PATH APP_DATA_DIR "/xcloud-library.json"
#define STEAM_LIBRARY_PATH APP_DATA_DIR "/steam-library.json"
#define LIBRARY_PATH (steam_link_selected() ? STEAM_LIBRARY_PATH : xcloud_selected() ? XBOX_LIBRARY_PATH \
                      : LIBRARY_CACHE_PATH)

/* Which games the account has in its library, kept apart from the list on
 * screen (a search replaces that): hashes of every store ID. */
#define LIBRARY_IDS_MAX (GFN_MAX_GAMES * 2)
static uint32_t g_library_ids[LIBRARY_IDS_MAX];
static unsigned g_library_id_count, g_library_games;
static bool g_library_known;
static LightLock g_library_lock = 1;

static uint32_t id_hash(const char *id)
{
    uint32_t hash = 2166136261u;
    for (; *id; ++id) hash = (hash ^ (uint8_t)*id) * 16777619u;
    return hash;
}

static void library_remember(const GfnClient *client)
{
    uint32_t ids[LIBRARY_IDS_MAX];
    unsigned count = 0;
    for (size_t i = 0; i < client->game_count; ++i) {
        const GfnGame *g = &client->games[i];
        if (count < LIBRARY_IDS_MAX && g->app_id[0]) ids[count++] = id_hash(g->app_id);
        for (unsigned v = 0; v < g->variant_count && count < LIBRARY_IDS_MAX; ++v)
            ids[count++] = id_hash(g->variants[v].id);
    }
    LightLock_Lock(&g_library_lock);
    memcpy(g_library_ids, ids, count * sizeof(ids[0]));
    g_library_id_count = count;
    g_library_games = (unsigned)client->game_count;
    g_library_known = true;
    LightLock_Unlock(&g_library_lock);
}

bool gfn_library_known(unsigned *games)
{
    LightLock_Lock(&g_library_lock);
    const bool known = g_library_known;
    if (games) *games = g_library_games;
    LightLock_Unlock(&g_library_lock);
    return known;
}

bool gfn_in_library(const GfnGame *game)
{
    if (!game) return false;
    uint32_t wanted[1 + GFN_MAX_VARIANTS];
    unsigned n = 0;
    wanted[n++] = id_hash(game->app_id);
    for (unsigned v = 0; v < game->variant_count; ++v) wanted[n++] = id_hash(game->variants[v].id);
    bool found = false;
    LightLock_Lock(&g_library_lock);
    for (unsigned i = 0; i < g_library_id_count && !found; ++i)
        for (unsigned k = 0; k < n && !found; ++k)
            found = g_library_ids[i] == wanted[k];
    LightLock_Unlock(&g_library_lock);
    return found;
}

/* The owned library is kept on the SD card so the next start shows it at
 * once; Refresh fetches it again. It holds titles, IDs and art URLs only. */
static void library_save(const GfnClient *client)
{
    library_remember(client);
    json_t *games = json_array();
    for (size_t i = 0; i < client->game_count; ++i) {
        const GfnGame *g = &client->games[i];
        json_t *variants = json_array();
        for (unsigned v = 0; v < g->variant_count; ++v)
            json_array_append_new(variants, json_pack("[s,s]", g->variants[v].id, g->variants[v].store));
        json_array_append_new(games, json_pack("{s:s,s:s,s:s,s:s,s:s,s:o,s:i}", "title", g->title,
                                               "id", g->app_id, "store", g->store,
                                               "image", g->image_url, "wide", g->wide_url, "variants", variants,
                                               "selected", (int)g->variant_selected));
    }
    json_t *root = json_pack("{s:I,s:o}", "saved_at", (json_int_t)client->library_saved_at,
                             "games", games);
    if (root) json_dump_file(root, LIBRARY_PATH, JSON_COMPACT);
    json_decref(root);
}

bool gfn_library_load(GfnClient *client)
{
    json_error_t error;
    json_t *root = json_load_file(LIBRARY_PATH, 0, &error);
    json_t *games = json_is_object(root) ? json_object_get(root, "games") : NULL;
    if (!json_is_array(games)) {
        json_decref(root);
        return false;
    }
    client->game_count = 0;
    size_t index; json_t *item;
    json_array_foreach(games, index, item) {
        if (client->game_count >= GFN_MAX_GAMES) break;
        GfnGame *g = &client->games[client->game_count];
        memset(g, 0, sizeof(*g));
        copy_json_string(g->title, sizeof(g->title), item, "title");
        copy_json_string(g->app_id, sizeof(g->app_id), item, "id");
        copy_json_string(g->store, sizeof(g->store), item, "store");
        copy_json_string(g->image_url, sizeof(g->image_url), item, "image");
        copy_json_string(g->wide_url, sizeof(g->wide_url), item, "wide");
        json_t *variants = json_object_get(item, "variants");
        size_t v; json_t *pair;
        json_array_foreach(variants, v, pair) {
            if (g->variant_count >= GFN_MAX_VARIANTS || !json_is_array(pair)) continue;
            json_t *id = json_array_get(pair, 0), *store = json_array_get(pair, 1);
            if (!json_is_string(id) || !json_is_string(store)) continue;
            snprintf(g->variants[g->variant_count].id, sizeof(g->variants[0].id), "%s", json_string_value(id));
            snprintf(g->variants[g->variant_count].store, sizeof(g->variants[0].store), "%s",
                     json_string_value(store));
            ++g->variant_count;
        }
        json_t *selected = json_object_get(item, "selected");
        if (json_is_integer(selected) && (unsigned)json_integer_value(selected) < g->variant_count)
            g->variant_selected = (unsigned)json_integer_value(selected);
        if (g->title[0] && g->app_id[0]) ++client->game_count;
    }
    json_t *saved = json_object_get(root, "saved_at");
    client->library_saved_at = json_is_integer(saved) ? json_integer_value(saved) : 0;
    client->catalog_total = client->game_count;
    json_decref(root);
    library_remember(client);
    /* Steam Link's Y opens the PCs instead of refreshing. */
    snprintf(client->status, sizeof(client->status), steam_link_selected() ? "Library: %lu games"
             : "Library: %lu games (Y refreshes)", (unsigned long)client->game_count);
    return true;
}

/* Latency: the second of two small requests to NVIDIA's session service,
 * so TLS setup is not counted. Throughput: a 768 KiB ranged download from
 * NVIDIA's CDN after a warm-up request on the same connection. */
bool gfn_connection_test(GfnClient *client)
{
    char base[96], info_url[128];
    provider_base_url(base, sizeof(base));
    snprintf(info_url, sizeof(info_url), "%s/v2/serverInfo", base);
    static const char *const cdn_url =
        "https://static.nvidiagrid.net/supported-public-game-list/locales/gfnpc-en-US.json";
    static const char *const warm_headers[] = { "Accept: */*", "Range: bytes=0-1023" };
    static const char *const range_headers[] = { "Accept: */*", "Range: bytes=0-786431" };
    static const char *const plain_headers[] = { "Accept: application/json" };
    client->conn_failed = true;
    client->conn_bars = osGetWifiStrength();
    HttpResponse response;
    unsigned best = 100000;
    for (int i = 0; i < 3; ++i) {
        const u64 start = osGetTime();
        const bool ok = http_request("GET", info_url, GFN_UA, plain_headers, 1, NULL, 64 * 1024, &response);
        const unsigned ms = (unsigned)(osGetTime() - start);
        http_response_free(&response);
        if (!ok) {
            snprintf(client->status, sizeof(client->status), "Connection check: NVIDIA unreachable");
            return false;
        }
        if (i > 0 && ms < best) best = ms;
    }
    client->conn_latency_ms = best;
    if (!http_request("GET", cdn_url, GFN_UA, warm_headers, 2, NULL, 64 * 1024, &response)) {
        http_response_free(&response);
        return false;
    }
    http_response_free(&response);
    const u64 start = osGetTime();
    http_request("GET", cdn_url, GFN_UA, range_headers, 2, NULL, 768 * 1024, &response);
    const u64 ms = osGetTime() - start;
    const size_t bytes = response.size;
    http_response_free(&response);
    if (bytes < 256 * 1024 || !ms) return false;
    client->conn_kbps = (unsigned)(bytes * 8 / ms);
    client->conn_tested_at = (int64_t)time(NULL);
    client->conn_failed = false;
    diagnostic_log("NET", "connection check bars=%u latency=%u ms download=%u kbps bytes=%lu",
                   client->conn_bars, client->conn_latency_ms, client->conn_kbps, (unsigned long)bytes);
    snprintf(client->status, sizeof(client->status), "Connection: %u ms, %u.%u Mbps, Wi-Fi %u/3",
             client->conn_latency_ms, client->conn_kbps / 1000, client->conn_kbps % 1000 / 100,
             client->conn_bars);
    return true;
}

bool gfn_fetch_library(GfnClient *client)
{
    if (steam_link_selected() ? !steam_link_fetch_library(client)
        : xcloud_selected() ? !xcloud_fetch_library(client) : !fetch_catalog(client, NULL, true)) return false;
    client->library_saved_at = (int64_t)time(NULL);
    library_save(client);
    return true;
}

bool gfn_search_catalog(GfnClient *client, const char *query)
{
    if (!query || !query[0]) {
        snprintf(client->status, sizeof(client->status), "Search text is empty");
        return false;
    }
    if (steam_link_selected()) return steam_link_search(client, query);
    if (xcloud_selected()) return xcloud_search(client, query);
    return fetch_catalog(client, query, false);
}

static void json_set_null(json_t *object, const char *key)
{
    json_object_set_new(object, key, json_null());
}

static char *build_session_body(const GfnGame *game, const char *device_id)
{
    char sub_session_id[40];
    generate_uuid(sub_session_id);
    json_t *root = json_object();
    json_t *request = json_object();
    json_t *features = json_object();
    json_t *metadata = json_array();
    json_t *monitors = json_array();
    json_t *monitor = json_object();
    if (!root || !request || !features || !metadata || !monitors || !monitor) goto fail;

    char *end = NULL;
    const long app_id = strtol(game->app_id, &end, 10);
    if (!game->app_id[0] || !end || *end != '\0' || app_id <= 0) goto fail;
    json_object_set_new(request, "appId", json_integer(app_id));
    json_object_set_new(request, "cmsId", json_string(game->app_id));
    json_set_null(request, "internalTitle");
    json_set_null(request, "networkTestSessionId");
    json_set_null(request, "parentSessionId");
    json_object_set_new(request, "clientIdentification", json_string("GFN-PC"));
    json_object_set_new(request, "deviceHashId", json_string(device_id));
    json_object_set_new(request, "clientVersion", json_string("30.0"));
    json_object_set_new(request, "clientPlatformName", json_string("windows"));
    json_object_set_new(request, "availableSupportedControllers", json_array());
    json_object_set_new(request, "sdkVersion", json_string("1.0"));
    json_object_set_new(request, "streamerVersion", json_integer(1));
    json_object_set_new(request, "useOps", json_true());
    json_object_set_new(request, "audioMode", json_integer(2));
    json_object_set_new(request, "sdrHdrMode", json_integer(0));
    json_set_null(request, "clientDisplayHdrCapabilities");
    json_object_set_new(request, "surroundAudioInfo", json_integer(0));
    json_object_set_new(request, "remoteControllersBitmap", json_integer(1));
    json_object_set_new(request, "clientTimezoneOffset", json_integer(0));
    json_object_set_new(request, "enhancedStreamMode", json_integer(1));
    json_object_set_new(request, "appLaunchMode", json_integer(2));
    json_object_set_new(request, "secureRTSPSupported", json_false());
    json_object_set_new(request, "partnerCustomData", json_string(""));
    json_object_set_new(request, "accountLinked", json_true());
    json_object_set_new(request, "enablePersistingInGameSettings", json_false());
    json_object_set_new(request, "userAge", json_integer(26));

    json_object_set_new(features, "reflex", json_false());
    json_object_set_new(features, "bitDepth", json_integer(0));
    json_object_set_new(features, "cloudGsync", json_false());
    json_object_set_new(features, "enabledL4S", json_false());
    json_object_set_new(features, "mouseMovementFlags", json_integer(0));
    json_object_set_new(features, "trueHdr", json_false());
    json_object_set_new(features, "supportedHidDevices", json_integer(0));
    json_object_set_new(features, "profile", json_integer(0));
    json_object_set_new(features, "fallbackToLogicalResolution", json_false());
    json_set_null(features, "hidDevices");
    json_object_set_new(features, "chromaFormat", json_integer(0));
    /* Server-side sharpening spends scarce bits on edges and noise; off by
     * default, like OpenNOW. Settings > Server sharpening brings it back. */
    json_object_set_new(features, "prefilterMode", json_integer(stream_profile_sharpen() ? 1 : 0));
    json_object_set_new(features, "prefilterSharpness", json_integer(stream_profile_sharpen() ? 50 : 0));
    json_object_set_new(features, "prefilterNoiseReduction", json_integer(0));
    json_object_set_new(features, "hudStreamingMode", json_integer(0));
    json_object_set_new(features, "sdrColorSpace", json_integer(2));
    json_object_set_new(features, "hdrColorSpace", json_integer(0));
    json_object_set_new(features, "maxBitrateKbps", json_integer(stream_profile_max_bitrate()));
    json_object_set_new(features, "codec", json_integer(1));
    json_object_set_new(features, "vsync", json_false());
    json_object_set_new(features, "dynamicStreamingMode", json_integer(stream_profile_dynamic_mode()));
    json_object_set_new(features, "audioChannelCount", json_integer(2));
    json_object_set_new(request, "requestedStreamingFeatures", features);
    features = NULL;

    json_array_append_new(metadata, json_pack("{s:s,s:s}", "key", "SubSessionId", "value", sub_session_id));
    json_array_append_new(metadata, json_pack("{s:s,s:s}", "key", "wssignaling", "value", "1"));
    json_array_append_new(metadata, json_pack("{s:s,s:s}", "key", "GSStreamerType", "value", "WebRTC"));
    json_array_append_new(metadata, json_pack("{s:s,s:s}", "key", "networkType", "value", "Unknown"));
    json_array_append_new(metadata, json_pack("{s:s,s:s}", "key", "ClientImeSupport", "value", "0"));
    char physical_resolution[96];
    snprintf(physical_resolution, sizeof(physical_resolution),
             "{\"horizontalPixels\":%u,\"verticalPixels\":%u}",
             stream_profile_width(), stream_profile_height());
    json_array_append_new(metadata, json_pack("{s:s,s:s}", "key", "clientPhysicalResolution", "value", physical_resolution));
    json_object_set_new(request, "metaData", metadata);
    metadata = NULL;

    json_object_set_new(monitor, "monitorId", json_integer(0));
    json_object_set_new(monitor, "positionX", json_integer(0));
    json_object_set_new(monitor, "positionY", json_integer(0));
    json_object_set_new(monitor, "widthInPixels", json_integer(stream_profile_width()));
    json_object_set_new(monitor, "heightInPixels", json_integer(stream_profile_height()));
    json_object_set_new(monitor, "framesPerSecond", json_integer((json_int_t)stream_profile_fps()));
    json_object_set_new(monitor, "sdrHdrMode", json_integer(0));
    json_set_null(monitor, "displayData");
    json_set_null(monitor, "hdr10PlusGamingData");
    json_object_set_new(monitor, "dpi", json_integer(100));
    json_array_append_new(monitors, monitor);
    monitor = NULL;
    json_object_set_new(request, "clientRequestMonitorSettings", monitors);
    monitors = NULL;
    json_object_set_new(root, "sessionRequestData", request);
    request = NULL;
    char *body = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    return body;

fail:
    if (monitor) json_decref(monitor);
    if (monitors) json_decref(monitors);
    if (metadata) json_decref(metadata);
    if (features) json_decref(features);
    if (request) json_decref(request);
    if (root) json_decref(root);
    return NULL;
}

static void flexible_json_text(char *destination, size_t size, json_t *value)
{
    if (json_is_string(value)) snprintf(destination, size, "%s", json_string_value(value));
    else if (json_is_integer(value)) snprintf(destination, size, "%lld", (long long)json_integer_value(value));
}

static int parse_session_status(json_t *value)
{
    if (json_is_integer(value)) return (int)json_integer_value(value);
    if (!json_is_string(value)) return -1;
    const char *text = json_string_value(value);
    if (!strcasecmp(text, "queued")) return 0;
    if (!strcasecmp(text, "provisioning") || !strcasecmp(text, "initializing") ||
        !strcasecmp(text, "setup") || !strcasecmp(text, "launching")) return 1;
    if (!strcasecmp(text, "active") || !strcasecmp(text, "ready") || !strcasecmp(text, "paused")) return 2;
    if (!strcasecmp(text, "streaming") || !strcasecmp(text, "playing") || !strcasecmp(text, "connected")) return 3;
    if (strstr(text, "fail") || strstr(text, "error") || strstr(text, "closed")) return 4;
    return -1;
}

static const char *connection_ip(json_t *connection)
{
    json_t *ip = json_object_get(connection, "ip");
    if (json_is_string(ip)) return json_string_value(ip);
    if (json_is_array(ip) && json_array_size(ip)) {
        json_t *first = json_array_get(ip, 0);
        if (json_is_string(first)) return json_string_value(first);
    }
    return "";
}

static int connection_port(json_t *connection)
{
    json_t *value = json_object_get(connection, "port");
    if (json_is_integer(value)) {
        const json_int_t port = json_integer_value(value);
        if (port > 0 && port <= 65535) return (int)port;
    }
    if (json_is_string(value)) {
        char *end = NULL;
        long port = strtol(json_string_value(value), &end, 10);
        if (port > 0 && port <= 65535 && end && *end == 0) return (int)port;
    }
    const char *path = "";
    value = json_object_get(connection, "resourcePath");
    if (json_is_string(value)) path = json_string_value(value);
    const char *scheme = strstr(path, "://");
    const char *host = scheme ? scheme + 3 : path;
    const char *slash = strchr(host, '/');
    const char *colon = strrchr(host, ':');
    if (colon && (!slash || colon < slash)) {
        char *end = NULL;
        long port = strtol(colon + 1, &end, 10);
        if (port > 0 && port <= 65535 && (!slash || end == slash)) return (int)port;
    }
    return 0;
}

static void parse_session_network(GfnClient *client, json_t *session)
{
    copy_json_string_if_present(client->session_token, sizeof(client->session_token), session, "sessionToken");
    copy_json_string_if_present(client->server_ip, sizeof(client->server_ip), session, "serverIp");
    copy_json_string_if_present(client->signaling_url, sizeof(client->signaling_url), session, "signalingUrl");
    json_t *connections = json_object_get(session, "connectionInfo");
    const int media_priorities[] = {2, 17, 14};
    for (size_t p = 0; p < sizeof(media_priorities) / sizeof(media_priorities[0]); ++p) {
        size_t index; json_t *connection;
        json_array_foreach(connections, index, connection) {
            json_t *usage_value = json_object_get(connection, "usage");
            int usage = json_is_integer(usage_value) ? (int)json_integer_value(usage_value) :
                        json_is_string(usage_value) ? atoi(json_string_value(usage_value)) : -1;
            if (usage != media_priorities[p]) continue;
            const char *ip = connection_ip(connection);
            const int port = connection_port(connection);
            if (port > 0 && (ip[0] || client->server_ip[0])) {
                snprintf(client->media_ip, sizeof(client->media_ip), "%s",
                         ip[0] ? ip : client->server_ip);
                client->media_port = port;
                break;
            }
        }
        if (client->media_ip[0] && client->media_port > 0) break;
    }
    size_t index; json_t *connection;
    json_array_foreach(connections, index, connection) {
        json_t *usage_value = json_object_get(connection, "usage");
        int usage = json_is_integer(usage_value) ? (int)json_integer_value(usage_value) :
                    json_is_string(usage_value) ? atoi(json_string_value(usage_value)) : -1;
        if (usage != 14) continue;
        const char *path = "";
        json_t *path_value = json_object_get(connection, "resourcePath");
        if (json_is_string(path_value)) path = json_string_value(path_value);
        const char *ip = connection_ip(connection);
        if (ip[0]) snprintf(client->server_ip, sizeof(client->server_ip), "%s", ip);
        if (!client->signaling_url[0]) {
            if (!strncmp(path, "wss://", 6)) {
                snprintf(client->signaling_url, sizeof(client->signaling_url), "%s", path);
            } else if (!strncmp(path, "https://", 8)) {
                snprintf(client->signaling_url, sizeof(client->signaling_url), "wss://%s", path + 8);
            } else if (!strncmp(path, "rtsps://", 8) || !strncmp(path, "rtsp://", 7)) {
                const char *host = strstr(path, "://");
                host = host ? host + 3 : path;
                size_t host_len = strcspn(host, "/:");
                snprintf(client->signaling_url, sizeof(client->signaling_url), "wss://%.*s/nvst/", (int)host_len, host);
            } else if (client->server_ip[0]) {
                snprintf(client->signaling_url, sizeof(client->signaling_url), "wss://%s:443%s",
                         client->server_ip, path[0] == '/' ? path : "/nvst/");
            }
        }
        break;
    }
}

/* The host and file type of an ad's media URL, never the full address. */
static void ad_media_summary(json_t *ad, char *out, size_t size)
{
    static const char *const keys[] = { "adUrl", "mediaUrl", "videoUrl", "url", "adMediaUrl" };
    out[0] = '\0';
    for (size_t k = 0; k < ARRAY_SIZE(keys); ++k) {
        json_t *value = json_object_get(ad, keys[k]);
        if (!json_is_string(value)) continue;
        const char *url = json_string_value(value);
        const char *host = strstr(url, "://");
        host = host ? host + 3 : url;
        const size_t host_len = strcspn(host, "/?#");
        const char *query = strpbrk(host, "?#");
        const size_t path_len = query ? (size_t)(query - host) : strlen(host);
        const char *dot = NULL;
        for (size_t i = host_len; i < path_len; ++i) if (host[i] == '.') dot = host + i;
        snprintf(out, size, "%s=%.*s ext=%.*s", keys[k], (int)(host_len < 60 ? host_len : 60), host,
                 dot ? (int)(path_len - (size_t)(dot - host) < 8 ? path_len - (size_t)(dot - host) : 8) : 1,
                 dot ? dot : "-");
        return;
    }
}

/* Free accounts get queue ads ("sessionAds"). Kasumi can't play video ads,
 * so it answers each one the way OpenNOW desktop does when an ad fails to
 * play (cancel, reason "error") and logs what NVIDIA does next: beta.18
 * showed free queues dropped ~120 s in, with the ads never answered. */
/* Field names of a queued session, and seatSetupInfo's numbers: NVIDIA
 * may carry the queue's state (ads, pauses) under names Kasumi doesn't read
 * yet. No values except numbers and booleans; never tokens. */
static void log_queue_shape(const GfnClient *client, json_t *root, json_t *session)
{
    static char logged_for[160];
    if (!strcmp(logged_for, client->session_id)) return;
    snprintf(logged_for, sizeof(logged_for), "%s", client->session_id);
    char line[480] = "";
    const char *key;
    json_t *value;
    json_object_foreach(root, key, value) {
        const size_t used = strlen(line);
        if (used + strlen(key) + 2 >= sizeof(line)) break;
        snprintf(line + used, sizeof(line) - used, "%s%s", used ? "," : "", key);
    }
    diagnostic_log("QUEUE", "response keys: %s", line);
    line[0] = '\0';
    json_object_foreach(session, key, value) {
        const size_t used = strlen(line);
        if (used + strlen(key) + 2 >= sizeof(line)) break;
        snprintf(line + used, sizeof(line) - used, "%s%s", used ? "," : "", key);
    }
    diagnostic_log("QUEUE", "session keys: %s", line);
    line[0] = '\0';
    json_t *seat = json_object_get(session, "seatSetupInfo");
    json_object_foreach(seat, key, value) {
        const size_t used = strlen(line);
        if (used + strlen(key) + 24 >= sizeof(line)) break;
        if (json_is_integer(value))
            snprintf(line + used, sizeof(line) - used, "%s%s=%lld", used ? " " : "", key, (long long)json_integer_value(value));
        else if (json_is_boolean(value))
            snprintf(line + used, sizeof(line) - used, "%s%s=%d", used ? " " : "", key, json_is_true(value));
        else
            snprintf(line + used, sizeof(line) - used, "%s%s:%s", used ? " " : "", key,
                     json_is_object(value) ? "obj" : json_is_array(value) ? "arr" : json_is_null(value) ? "null" : "str");
    }
    diagnostic_log("QUEUE", "seatSetupInfo %s", line);
    json_t *idle = json_object_get(session, "userIdleWarningTimeoutInMs");
    json_t *gpu = json_object_get(session, "gpuType");
    diagnostic_log("QUEUE", "idleWarning=%lld gpu=%.24s", json_is_integer(idle) ? (long long)json_integer_value(idle) : -1,
                   json_is_string(gpu) ? json_string_value(gpu) : "-");
    /* Anything that sounds like ads, progress or an opportunity. */
    json_object_foreach(session, key, value) {
        if (!strstr(key, "ad") && !strstr(key, "Ad") && !strstr(key, "pportun") && !strstr(key, "rogress") &&
            !strstr(key, "ueue") && !strstr(key, "aused"))
            continue;
        if (json_is_integer(value))
            diagnostic_log("QUEUE", "%s=%lld", key, (long long)json_integer_value(value));
        else if (json_is_boolean(value))
            diagnostic_log("QUEUE", "%s=%d", key, json_is_true(value));
        else if (json_is_array(value))
            diagnostic_log("QUEUE", "%s: array of %u", key, (unsigned)json_array_size(value));
        else if (json_is_object(value)) {
            char inner[240] = "";
            const char *k2;
            json_t *v2;
            json_object_foreach(value, k2, v2) {
                const size_t used = strlen(inner);
                if (used + strlen(k2) + 24 >= sizeof(inner)) break;
                if (json_is_integer(v2))
                    snprintf(inner + used, sizeof(inner) - used, "%s%s=%lld", used ? " " : "", k2, (long long)json_integer_value(v2));
                else if (json_is_boolean(v2))
                    snprintf(inner + used, sizeof(inner) - used, "%s%s=%d", used ? " " : "", k2, json_is_true(v2));
                else
                    snprintf(inner + used, sizeof(inner) - used, "%s%s", used ? " " : "", k2);
            }
            diagnostic_log("QUEUE", "%s {%s}", key, inner);
        } else {
            diagnostic_log("QUEUE", "%s: %s", key, json_is_null(value) ? "null" : "text");
        }
    }
}

static void session_rig_host(json_t *session, char *out, size_t size);

static void log_other_sessions(const GfnClient *client, json_t *root)
{
    json_t *others = json_object_get(root, "otherUserSessions");
    char own_app[24] = "";
    json_t *own = json_object_get(root, "session");
    json_t *own_request = json_is_object(own) ? json_object_get(own, "sessionRequestData") : NULL;
    flexible_json_text(own_app, sizeof(own_app), json_is_object(own_request) ? json_object_get(own_request, "appId") : NULL);
    static char last[400];
    char line[400] = "";
    size_t index;
    json_t *other;
    json_array_foreach(others, index, other) {
        if (index >= 4) break;
        char id[160] = "", app[24] = "", host[128] = "";
        flexible_json_text(id, sizeof(id), json_object_get(other, "sessionId"));
        json_t *request = json_object_get(other, "sessionRequestData");
        flexible_json_text(app, sizeof(app), json_is_object(request) ? json_object_get(request, "appId") : NULL);
        session_rig_host(other, host, sizeof(host));
        json_t *seat = json_object_get(other, "seatSetupInfo");
        json_t *place = json_is_object(seat) ? json_object_get(seat, "queuePosition") : NULL;
        const size_t used = strlen(line);
        snprintf(line + used, sizeof(line) - used, "%s[status=%d ours=%d sameGame=%d place=%lld rig=%s id=%.8s]",
                 used ? " " : "", parse_session_status(json_object_get(other, "status")),
                 id[0] && !strcmp(id, client->session_id), app[0] && !strcmp(app, own_app),
                 json_is_integer(place) ? (long long)json_integer_value(place) : -1,
                 host[0] ? "named" : "none", id);
    }
    char head[48];
    snprintf(head, sizeof(head), "count=%u ", json_is_array(others) ? (unsigned)json_array_size(others) : 0);
    char both[sizeof(last)];
    snprintf(both, sizeof(both), "%s%.300s", head, line);
    if (!strcmp(last, both)) return;
    snprintf(last, sizeof(last), "%s", both);
    diagnostic_log("QUEUE", "other sessions %s%s", head, line[0] ? line : "(none)");
}

static void note_session_ads(GfnClient *client, json_t *session)
{
    json_t *ads = json_object_get(session, "sessionAds");
    json_t *progress = json_object_get(session, "sessionProgress");
    json_t *opportunity = json_object_get(session, "opportunity");
    bool required = json_is_true(json_object_get(session, "sessionAdsRequired")) ||
                    json_is_true(json_object_get(session, "isAdsRequired")) ||
                    (json_is_object(progress) && json_is_true(json_object_get(progress, "isAdsRequired")));
    const size_t count = json_is_array(ads) ? json_array_size(ads) : 0;
    if (count) required = true;
    const bool paused = json_is_object(opportunity) && json_is_true(json_object_get(opportunity, "queuePaused"));
    json_t *grace = json_is_object(opportunity) ? json_object_get(opportunity, "gracePeriodSeconds") : NULL;
    static unsigned logged_count = ~0u;
    static bool logged_paused, logged_required;
    if (required != logged_required || count != logged_count || paused != logged_paused) {
        logged_required = required;
        logged_count = (unsigned)count;
        logged_paused = paused;
        diagnostic_log("QUEUEAD", "required=%d ads=%u queuePaused=%d grace=%lld opportunity=%d",
                       required, (unsigned)count, paused,
                       json_is_integer(grace) ? (long long)json_integer_value(grace) : -1,
                       json_is_object(opportunity));
        size_t index;
        json_t *ad;
        json_array_foreach(ads, index, ad) {
            if (index >= 3) break;
            char keys[160] = "";
            const char *key;
            json_t *value;
            json_object_foreach(ad, key, value) {
                const size_t used = strlen(keys);
                if (used + strlen(key) + 2 >= sizeof(keys)) break;
                snprintf(keys + used, sizeof(keys) - used, "%s%s", used ? "," : "", key);
            }
            char media[112];
            ad_media_summary(ad, media, sizeof(media));
            json_t *seconds = json_object_get(ad, "adLengthInSeconds");
            json_t *ms = json_object_get(ad, "durationMs");
            diagnostic_log("QUEUEAD", "ad %u keys=%s length=%.1fs %s", (unsigned)index, keys,
                           json_is_number(seconds) ? json_number_value(seconds) :
                           json_is_integer(ms) ? (double)json_integer_value(ms) / 1000.0 : -1.0,
                           media[0] ? media : "media=none");
        }
    }
    client->ads_required = client->ads_required || required;
    size_t index;
    json_t *ad;
    json_array_foreach(ads, index, ad) {
        char id[64] = "";
        flexible_json_text(id, sizeof(id), json_object_get(ad, "adId"));
        if (!id[0]) continue;
        bool known = false;
        for (unsigned i = 0; i < client->ads_pending_count; ++i) known = known || !strcmp(client->ads_pending[i], id);
        /* Every ad seen; the first `ads_answered` of them are answered. */
        if (!known && client->ads_pending_count < ARRAY_SIZE(client->ads_pending))
            snprintf(client->ads_pending[client->ads_pending_count++], sizeof(client->ads_pending[0]), "%s", id);
    }
}

static bool rig_base(const char *host, char *out, size_t size);

/* Where to reach the session now: its control server, else where it was made. */
static const char *session_control(const GfnClient *client)
{
    return client->session_control_url[0] ? client->session_control_url : client->session_base_url;
}

static bool apply_session_response(GfnClient *client, HttpResponse *response, const char *operation)
{
    json_error_t error;
    json_t *root = json_loadb(response->body ? response->body : "", response->size, 0, &error);
    if (!root) {
        snprintf(client->status, sizeof(client->status), "%s: invalid JSON", operation);
        diagnostic_flag("bad-json", "%s http=%ld bytes=%lu line=%d %.60s", operation, response->status,
                        (unsigned long)response->size, error.line, error.text);
        client->session_state = GFN_SESSION_ERROR;
        return false;
    }
    json_t *request_status = json_object_get(root, "requestStatus");
    json_t *code_value = json_is_object(request_status) ? json_object_get(request_status, "statusCode") : NULL;
    const int status_code = json_is_integer(code_value) ? (int)json_integer_value(code_value) : -1;
    if (response->status < 200 || response->status >= 300 || status_code != 1) {
        json_t *reason_value = json_is_object(request_status) ? json_object_get(request_status, "statusDescription") : NULL;
        const char *reason = json_is_string(reason_value) ? json_string_value(reason_value) : "";
        /* NVIDIA's reasons, in words a player can act on. */
        const char *code = "http";
        if (strstr(reason, "LIMITED_MODE")) {
            /* Passes by itself: beta.23 consoles refused this way played
             * minutes later. */
            code = "limited";
            snprintf(client->status, sizeof(client->status),
                     "NVIDIA is limiting new sessions right now (limited mode). It usually passes in a few "
                     "minutes; try again soon (code %d).", status_code);
        } else if (strstr(reason, "REGION_NOT_SUPPORTED")) {
            code = "region";
            char region[40];
            regions_last_used(region, sizeof(region));
            snprintf(client->status, sizeof(client->status),
                     "The %.24s server isn't open to this account. Abroad? Pick a server in your home "
                     "country in Settings > Network (code %d).", region[0] ? region : "chosen", status_code);
        } else if (strstr(reason, "ENTITLEMENT")) {
            code = "entitlement";
            GfnProvider provider;
            provider_active(&provider);
            if (!provider_is_nvidia())
                snprintf(client->status, sizeof(client->status),
                         "This account has no %.20s access. For an NVIDIA account, pick NVIDIA in "
                         "Settings > Account and sign in again (code %d).", provider.name, status_code);
            else {
                /* Where NVIDIA runs GeForce NOW itself (beta.22 report
                 * DSZSUT, Spain) a partner is no answer: the account is. */
                GfnProvider local;
                providers_recommended(&local);
                if (strcmp(local.code, PROVIDER_NVIDIA)) {
                    char country[4];
                    providers_country(country, sizeof(country));
                    snprintf(client->status, sizeof(client->status),
                             "This account can't stream in %.20s. If %.20s sold you GeForce NOW, pick it in "
                             "Settings > Account and sign in again (code %d).", providers_country_name(country),
                             local.name, status_code);
                }
                else
                    snprintf(client->status, sizeof(client->status),
                             "NVIDIA says this account can't play this game. Steam, Epic and Ubisoft games "
                             "need that store linked at play.geforcenow.com (no VPN) (code %d).", status_code);
            }
        } else if (strstr(reason, "NO_CAPACITY") || strstr(reason, "CAPACITY")) {
            code = "capacity";
            snprintf(client->status, sizeof(client->status),
                     "NVIDIA has no free rigs for this right now. Try again in a few minutes (code %d).", status_code);
        } else if (strstr(reason, "SESSION_LIMIT") || strstr(reason, "MAX_SESSION")) {
            code = "limit";
            snprintf(client->status, sizeof(client->status),
                     "Another GeForce NOW session is running on this account. Close it and try again (code %d).",
                     status_code);
        } else if (strstr(reason, "ABANDONED")) {
            code = "abandoned";
            snprintf(client->status, sizeof(client->status),
                     "NVIDIA dropped this place in the queue. Press A to queue again (code %d).", status_code);
        } else if (strstr(reason, "MAINTENANCE")) {
            code = "maintenance";
            snprintf(client->status, sizeof(client->status),
                     "NVIDIA has this game in maintenance right now. Try again later or play another game "
                     "(code %d).", status_code);
        } else {
            snprintf(client->status, sizeof(client->status), "%s: CloudMatch HTTP %ld code %d %.60s",
                     operation, response->status, status_code, reason);
            /* An answer with no wording of ours: the player saw raw codes. */
            diagnostic_flag("unmapped-reason", "%s http=%ld code=%d desc=%.80s", operation, response->status,
                            status_code, reason[0] ? reason : "-");
        }
        snprintf(client->fail_code, sizeof(client->fail_code), "%s", code);
        client->session_state = GFN_SESSION_ERROR;
        json_decref(root);
        return false;
    }
    json_t *session = json_object_get(root, "session");
    if (!json_is_object(session)) {
        const char *description = "CloudMatch rejected request";
        json_t *description_value = json_is_object(request_status) ? json_object_get(request_status, "statusDescription") : NULL;
        if (json_is_string(description_value)) description = json_string_value(description_value);
        snprintf(client->status, sizeof(client->status), "%s: code %d %.90s", operation, status_code, description);
        diagnostic_flag("no-session", "%s http=%ld code=%d %.80s", operation, response->status, status_code,
                        description);
        client->session_state = GFN_SESSION_ERROR;
        json_decref(root);
        return false;
    }
    flexible_json_text(client->session_id, sizeof(client->session_id), json_object_get(session, "sessionId"));
    if (!client->session_id[0]) {
        snprintf(client->status, sizeof(client->status), "%s: CloudMatch response has no session ID", operation);
        diagnostic_flag("no-session", "%s: no sessionId (http=%ld)", operation, response->status);
        client->session_state = GFN_SESSION_ERROR;
        json_decref(root);
        return false;
    }
    client->session_status = parse_session_status(json_object_get(session, "status"));
    json_t *queue = json_object_get(session, "queuePosition");
    client->queue_session_position = json_is_integer(queue) ? (int)json_integer_value(queue) : -1;
    json_t *seat = json_object_get(session, "seatSetupInfo");
    json_t *seat_queue = json_is_object(seat) ? json_object_get(seat, "queuePosition") : NULL;
    client->queue_seat_position = json_is_integer(seat_queue) ? (int)json_integer_value(seat_queue) : -1;
    json_t *root_queue = json_object_get(root, "queuePosition");
    client->queue_root_position = json_is_integer(root_queue) ? (int)json_integer_value(root_queue) : -1;
    /* Match current OpenNOW precedence: session value, then seat setup, with
     * the root field authoritative when NVIDIA supplies it. */
    int reported = client->queue_session_position;
    if (client->queue_seat_position >= 0) reported = client->queue_seat_position;
    if (client->queue_root_position >= 0) reported = client->queue_root_position;
    json_t *step = json_is_object(seat) ? json_object_get(seat, "seatSetupStep") : NULL;
    client->seat_setup_step = json_is_integer(step) ? (int)json_integer_value(step) : -1;
    /* Hardware log (build 47): the seat position counted 4-3-2, then jumped
     * back to 4 for ~25 s as the response grew, before the rig was ready.
     * That later number is a setup stage, not a place in line. Lock onto the
     * step the queue started in (a new step means rig setup), and within it
     * never let the shown position rise. */
    bool queued = reported > 0;
    /* Step 1 is the line itself. Beta.27 report SAFA4K: a brief "1" at step
     * 5 locked the count there, and the real queue (87 down to 15) never
     * showed. Lock again onto step 1 when it comes. */
    if (queued && client->seat_setup_step == 1 && client->queue_step != 1) client->queue_best = 0;
    if (queued && client->queue_best <= 0) {
        client->queue_step = client->seat_setup_step;
        client->queue_best = reported;
    } else if (queued && client->seat_setup_step != client->queue_step) {
        queued = false;
    } else if (queued && reported < client->queue_best) {
        client->queue_best = reported;
    }
    client->queue_position = queued ? client->queue_best : 0;
    client->queue_reported = reported;
    diagnostic_log("CLOUDMATCH", "queue shown=%d reported=%d session=%d seat=%d root=%d step=%d queueStep=%d best=%d status=%d",
                   client->queue_position, reported, client->queue_session_position,
                   client->queue_seat_position, client->queue_root_position,
                   client->seat_setup_step, client->queue_step, client->queue_best,
                   client->session_status);
    parse_session_network(client, session);
    /* Follow the session to the zone that holds it (the web client polls
     * sessionControlInfo.ip, which changes while queued). */
    json_t *control = json_object_get(session, "sessionControlInfo");
    const char *control_host = json_is_object(control) ? connection_ip(control) : "";
    char control_url[256];
    if (control_host[0] && rig_base(control_host, control_url, sizeof(control_url)) &&
        strcmp(control_url, client->session_control_url)) {
        diagnostic_log("CLOUDMATCH", "session control server now %.80s", control_host);
        snprintf(client->session_control_url, sizeof(client->session_control_url), "%s", control_url);
    }
    if (client->queue_position > 0) log_queue_shape(client, root, session);
    if (json_object_get(root, "otherUserSessions")) log_other_sessions(client, root);
    note_session_ads(client, session);
    const bool ad_required = client->ads_required;
    const int64_t now = (int64_t)time(NULL);
    const bool resuming = client->resuming_until && now < client->resuming_until;
    client->session_paused = false;
    if ((client->session_status == 4 || client->session_status == 5) && !resuming) {
        /* OpenNOW desktop: 4 and 5 are a paused seat (the stream dropped),
         * which a RESUME brings back; beta.18 treated 4 as the end. */
        client->session_paused = true;
        client->session_state = GFN_SESSION_ERROR;
        snprintf(client->fail_code, sizeof(client->fail_code), "paused");
        snprintf(client->status, sizeof(client->status), "NVIDIA paused this session (status %d)",
                 client->session_status);
    } else if (client->session_status == 4 || client->session_status == 5 || client->session_status == 6) {
        client->session_state = GFN_SESSION_SETUP;
        snprintf(client->status, sizeof(client->status), "Resuming your game...");
    } else if ((client->session_status == 2 || client->session_status == 3) && client->signaling_url[0]) {
        client->session_state = GFN_SESSION_READY;
        client->resuming_until = 0;
        snprintf(client->status, sizeof(client->status), "Session ready; signaling endpoint received");
    } else if (resuming) {
        client->session_state = GFN_SESSION_SETUP;
        snprintf(client->status, sizeof(client->status), "Resuming your game...");
    } else if (client->session_status >= 7) {
        /* Over (OpenNOW desktop: 7+ is "no longer resumable"). Beta.25 test:
         * an idle game came back as status 7 and Kasumi waited on it as if
         * it were still starting, until the player left. */
        client->session_state = GFN_SESSION_ERROR;
        snprintf(client->fail_code, sizeof(client->fail_code), "ended");
        /* Beta.26 reports (9WFFRT, QYHU2H): players who quit from the game's
         * own menu landed here, with a message blaming idling or another
         * device. NVIDIA's reason is in session.errorCode; log it and say
         * only what we know. */
        char reason[24];
        flexible_json_text(reason, sizeof(reason), json_object_get(session, "errorCode"));
        snprintf(client->end_error_code, sizeof(client->end_error_code), "%s", reason);
        json_t *end_desc = json_is_object(request_status) ? json_object_get(request_status, "statusDescription") : NULL;
        json_t *end_unified = json_is_object(request_status) ? json_object_get(request_status, "unifiedErrorCode") : NULL;
        /* errorCode 1 is a clean end, mostly the game closing (its own
         * Exit or Save and quit): NVIDIA ends the rig with it, as on PC. */
        snprintf(client->status, sizeof(client->status), !strcmp(reason, "1") ?
                 "The game closed, so NVIDIA ended the cloud session. You're still signed in: press A to play again, or B to go back." :
                 "The game session has ended. Press A to start it again, or B to go back.");
        diagnostic_log("CLOUDMATCH", "%s: session over (status %d) errorCode=%s desc=%.60s unified=%lld",
                       operation, client->session_status, reason[0] ? reason : "-",
                       json_is_string(end_desc) ? json_string_value(end_desc) : "-",
                       json_is_integer(end_unified) ? (long long)json_integer_value(end_unified) : -1);
        /* Which fields NVIDIA sends with an end, to find why a session ended
         * (values left out: some hold addresses). */
        char keys[200] = "";
        const char *key;
        json_t *value;
        json_object_foreach(session, key, value) {
            const size_t used = strlen(keys);
            if (used + strlen(key) + 2 >= sizeof(keys)) break;
            snprintf(keys + used, sizeof(keys) - used, "%s%s", used ? "," : "", key);
        }
        diagnostic_log("CLOUDMATCH", "end fields: %s", keys);
        json_decref(root);
        return false;
    } else if (client->queue_position > 0 ||
               (client->session_status == 0 && client->queue_best <= 0)) {
        client->session_state = GFN_SESSION_QUEUED;
        if (client->queue_position >= 0)
            snprintf(client->status, sizeof(client->status), "GFN queue position: %d%s",
                     client->queue_position, ad_required ? " (ad required)" : "");
        else
            snprintf(client->status, sizeof(client->status), "GFN queued; position not supplied%s",
                     ad_required ? " (ad required)" : "");
    } else {
        client->session_state = GFN_SESSION_SETUP;
        snprintf(client->status, sizeof(client->status), "%s: provisioning (status %d)%s",
                 operation, client->session_status, ad_required ? ", ad required" : "");
    }
    client->next_session_poll_at = (int64_t)time(NULL) + 2;
    json_decref(root);
    return client->session_id[0] != '\0';
}

static size_t cloudmatch_headers(GfnClient *client, const char **headers,
                                 char client_header[80], char device_header[80])
{
    snprintf(g_authorization_header, sizeof(g_authorization_header),
             "Authorization: GFNJWT %s", gfn_bearer_token(client));
    snprintf(client_header, 80, "nv-client-id: %s", client->session_client_id);
    snprintf(device_header, 80, "x-device-id: %s", client->session_device_id);
    const char *values[] = {
        g_authorization_header, "Content-Type: application/json", client_header,
        "nv-browser-type: CHROME", "nv-client-streamer: NVIDIA-CLASSIC",
        "nv-client-type: NATIVE", "nv-client-version: 2.0.87.131",
        "nv-device-make: UNKNOWN", "nv-device-model: UNKNOWN",
        "nv-device-os: WINDOWS", "nv-device-type: DESKTOP", device_header,
        "Origin: https://play.geforcenow.com", "Referer: https://play.geforcenow.com/",
        "Connection: close"
    };
    memcpy(headers, values, sizeof(values));
    return ARRAY_SIZE(values);
}

static void cloudmatch_log_response(const char *operation, const HttpResponse *response)
{
    json_error_t error;
    json_t *root = json_loadb(response->body ? response->body : "",
                              response->size, 0, &error);
    if (!root) {
        diagnostic_log("CLOUDMATCH", "%s http=%ld bytes=%lu non-json line=%d",
                       operation, response->status,
                       (unsigned long)response->size, error.line);
        return;
    }

    json_t *request_status = json_object_get(root, "requestStatus");
    json_t *code = json_is_object(request_status)
        ? json_object_get(request_status, "statusCode") : NULL;
    json_t *description = json_is_object(request_status)
        ? json_object_get(request_status, "statusDescription") : NULL;
    json_t *unified = json_is_object(request_status)
        ? json_object_get(request_status, "unifiedErrorCode") : NULL;
    json_t *session = json_object_get(root, "session");
    json_t *session_error = json_is_object(session)
        ? json_object_get(session, "errorCode") : NULL;
    json_t *session_status = json_is_object(session)
        ? json_object_get(session, "status") : NULL;

    diagnostic_log("CLOUDMATCH",
                   "%s http=%ld bytes=%lu code=%lld desc=%.80s unified=%lld sessionStatus=%lld sessionError=%lld",
                   operation, response->status, (unsigned long)response->size,
                   json_is_integer(code) ? (long long)json_integer_value(code) : -1,
                   json_is_string(description) ? json_string_value(description) : "missing",
                   json_is_integer(unified) ? (long long)json_integer_value(unified) : -1,
                   json_is_integer(session_status) ? (long long)json_integer_value(session_status) : -1,
                   json_is_integer(session_error) ? (long long)json_integer_value(session_error) : -1);
    json_decref(root);
}

/* ---- Leftover sessions and the per-device limit ------------------------------
 *
 * CloudMatch counts every session in queue, setup, play or pause against a
 * limit per device. Beta.18 reports: 30 consoles refused with
 * SESSION_LIMIT_PER_DEVICE_EXCEEDED, and every DELETE of the session the
 * refusal named answered 404 (a new client id per launch, and the DELETE sent
 * to region servers instead of the rig that owns it). The slot then freed
 * itself 12 s to 8 min later, while the quick retries drew 429s. Now, like
 * OpenNOW desktop: the refusal names the session and its rig; the player
 * chooses Resume or End; End deletes it on its rig; and when nothing can be
 * closed the launch waits for NVIDIA on a slow timer (limit_wait). */

#define ZOMBIE_MAX 8

typedef struct {
    const char *bases[3];
    unsigned base_count;
} CloudBases;

/* The rig first (when known), then the session's server, then NVIDIA's. */
static CloudBases cloud_bases(const GfnClient *client, const char *rig)
{
    CloudBases b = { { NULL, NULL, NULL }, 0 };
    if (rig && rig[0]) b.bases[b.base_count++] = rig;
    if (client->session_base_url[0] && (!rig || strcmp(rig, client->session_base_url)))
        b.bases[b.base_count++] = client->session_base_url;
    /* The provider's own entry point (worker thread only). */
    static char entry[96];
    provider_base_url(entry, sizeof(entry));
    if (strcmp(client->session_base_url, entry)) b.bases[b.base_count++] = entry;
    return b;
}

/* "https://host" for a rig NVIDIA named, only for NVIDIA hosts and public
 * addresses (the name comes from a server response). */
static bool rig_base(const char *host, char *out, size_t size)
{
    out[0] = '\0';
    if (!host || !host[0] || strlen(host) > 100 || strpbrk(host, "/:?#@ \\")) return false;
    const size_t n = strlen(host);
    const bool nvidia = n > 15 && !strcasecmp(host + n - 15, ".nvidiagrid.net");
    unsigned a = 0, b = 0, c = 0, d = 0;
    char extra;
    const bool ipv4 = sscanf(host, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) == 4 &&
                      a < 256 && b < 256 && c < 256 && d < 256;
    const bool local = ipv4 && (a == 0 || a == 10 || a == 127 || a >= 224 || (a == 169 && b == 254) ||
                                (a == 172 && b >= 16 && b < 32) || (a == 192 && b == 168) ||
                                (a == 100 && b >= 64 && b < 128));
    if (!nvidia && (!ipv4 || local)) return false;
    snprintf(out, size, "https://%s", host);
    return true;
}

/* The server that controls a session (its seat), as CloudMatch names it. */
static void session_rig_host(json_t *session, char *out, size_t size)
{
    out[0] = '\0';
    json_t *control = json_object_get(session, "sessionControlInfo");
    if (json_is_object(control)) snprintf(out, size, "%s", connection_ip(control));
    if (out[0]) return;
    json_t *connections = json_object_get(session, "connectionInfo");
    size_t index;
    json_t *connection;
    json_array_foreach(connections, index, connection) {
        json_t *usage = json_object_get(connection, "usage");
        if (!json_is_integer(usage) || json_integer_value(usage) != 14) continue;
        snprintf(out, size, "%s", connection_ip(connection));
        if (out[0]) return;
    }
    json_t *server = json_object_get(session, "serverIp");
    if (json_is_string(server)) snprintf(out, size, "%s", json_string_value(server));
}

/* Active session IDs on one server (for the log: which sessions CloudMatch
 * shows this client). */
static unsigned cloudmatch_list_active(const char *base, const char **headers, size_t header_count)
{
    char url[384];
    snprintf(url, sizeof(url), "%s/v2/session", base);
    HttpResponse response;
    if (!http_request("GET", url, GFN_UA, headers, header_count, NULL, 1024 * 1024, &response)) return 0;
    unsigned count = 0;
    if (response.status >= 200 && response.status < 300) {
        json_error_t error;
        json_t *root = json_loadb(response.body ? response.body : "", response.size, 0, &error);
        json_t *sessions = root ? json_object_get(root, "sessions") : NULL;
        size_t index;
        json_t *session;
        json_array_foreach(sessions, index, session) {
            const int status = parse_session_status(json_object_get(session, "status"));
            if (status >= 0 && status <= 6) ++count;
        }
        json_decref(root);
    }
    http_response_free(&response);
    return count;
}

/* DELETE on each server until one accepts. True only for a real 2xx: a 404
 * everywhere means CloudMatch would not let this client touch it, so the
 * slot is not freed (beta.18 counted that as success and retried at once). */
static bool cloudmatch_stop_anywhere(const CloudBases *bases, const char *id, const char **headers,
                                     size_t header_count)
{
    for (unsigned i = 0; i < bases->base_count; ++i) {
        char url[512];
        snprintf(url, sizeof(url), "%s/v2/session/%s", bases->bases[i], id);
        HttpResponse response;
        if (!http_request("DELETE", url, GFN_UA, headers, header_count, NULL, 256 * 1024, &response)) {
            diagnostic_log("CLOUDMATCH", "stop other session at server %u: no connection", i);
            continue;
        }
        const long status = response.status;
        http_response_free(&response);
        const char *host = strstr(bases->bases[i], "://");
        diagnostic_log("CLOUDMATCH", "stop other session at %.60s: http=%ld",
                       host ? host + 3 : bases->bases[i], status);
        if (status >= 200 && status < 300) return true;
    }
    return false;
}

/* Up to ~24 s for CloudMatch to release a stopped session. */
static void cloudmatch_wait_clear(const CloudBases *bases, const char **headers, size_t header_count)
{
    for (int check = 1; check <= 8; ++check) {
        svcSleepThread(3000000000LL);
        unsigned remaining = 0;
        for (unsigned i = 0; i < bases->base_count; ++i)
            remaining += cloudmatch_list_active(bases->bases[i], headers, header_count);
        if (!remaining) {
            diagnostic_log("CLOUDMATCH", "no sessions listed after %ds", check * 3);
            return;
        }
        diagnostic_log("CLOUDMATCH", "waiting for %u session(s) to close", remaining);
    }
}

static bool is_session_limit(const HttpResponse *response)
{
    if (!response->body) return false;
    return strstr(response->body, "SESSION_LIMIT") != NULL || strstr(response->body, "4AF1201E") != NULL;
}

/* The session a SESSION_LIMIT refusal names in "otherUserSessions": its id,
 * rig, game and state. The refusal's own "session" is this launch echoed
 * back (status 1, the requested game, never created): beta.18 and beta.19
 * tried to close or resume it and always got 404. A per-device refusal
 * often names no other session at all; then NVIDIA is still closing this
 * console's last one, and the launch just waits. */
static bool capture_conflict(GfnClient *client, const HttpResponse *refusal, const GfnGame *game)
{
    memset(&client->conflict, 0, sizeof(client->conflict));
    json_error_t error;
    json_t *root = json_loadb(refusal->body ? refusal->body : "", refusal->size, 0, &error);
    json_t *candidates[ZOMBIE_MAX];
    unsigned count = 0;
    json_t *others = root ? json_object_get(root, "otherUserSessions") : NULL;
    size_t index;
    json_t *other;
    json_array_foreach(others, index, other) {
        if (count < ZOMBIE_MAX && json_is_object(other)) candidates[count++] = other;
    }
    unsigned named = 0;
    for (unsigned i = 0; i < count; ++i) {
        char id[160] = "";
        flexible_json_text(id, sizeof(id), json_object_get(candidates[i], "sessionId"));
        if (!id[0]) continue;
        ++named;
        if (client->conflict.id[0]) continue;
        snprintf(client->conflict.id, sizeof(client->conflict.id), "%s", id);
        session_rig_host(candidates[i], client->conflict.host, sizeof(client->conflict.host));
        json_t *request = json_object_get(candidates[i], "sessionRequestData");
        flexible_json_text(client->conflict.app_id, sizeof(client->conflict.app_id),
                           json_is_object(request) ? json_object_get(request, "appId") : NULL);
        client->conflict.status = parse_session_status(json_object_get(candidates[i], "status"));
    }
    json_decref(root);
    char rig[160];
    diagnostic_log("CLOUDMATCH", "session limit: %u other session(s) named; first status=%d sameGame=%d rig=%s",
                   named, client->conflict.status,
                   client->conflict.app_id[0] && game && !strcmp(client->conflict.app_id, game->app_id),
                   rig_base(client->conflict.host, rig, sizeof(rig)) ? client->conflict.host :
                   client->conflict.host[0] ? "untrusted" : "none");
    return client->conflict.id[0] != '\0';
}

/* Beta.18 and older made a new client id for every launch and saved it with
 * the running session. CloudMatch only lets that id close the session, so it
 * is kept for this run to try on sessions that refuse the fixed id. */
static char g_legacy_client_id[40];

/* DELETE `id` as another client id (an older build's). */
static bool cloudmatch_stop_as(GfnClient *client, const CloudBases *bases, const char *id, const char *client_id)
{
    char saved[sizeof(client->session_client_id)];
    memcpy(saved, client->session_client_id, sizeof(saved));
    snprintf(client->session_client_id, sizeof(client->session_client_id), "%s", client_id);
    const char *headers[16]; char client_header[80], device_header[80];
    const size_t count = cloudmatch_headers(client, headers, client_header, device_header);
    const bool stopped = cloudmatch_stop_anywhere(bases, id, headers, count);
    memcpy(client->session_client_id, saved, sizeof(saved));
    return stopped;
}

/* The session this console left behind (crash, power loss), if any: stop it
 * where it was made. Only our own: an account's other sessions may be a game
 * running on the player's PC, which is theirs to end (the Resume / End
 * question). */
static void cloudmatch_stop_remembered(GfnClient *client, const char **headers, size_t header_count)
{
    json_error_t error;
    json_t *root = json_load_file(ACTIVE_SESSION_PATH, 0, &error);
    char id[160] = "", base[256] = "", client_id[40] = "";
    if (json_is_object(root)) {
        copy_json_string(id, sizeof(id), root, "session_id");
        copy_json_string(base, sizeof(base), root, "base_url");
        copy_json_string(client_id, sizeof(client_id), root, "client_id");
    }
    json_decref(root);
    if (!id[0]) return;
    const CloudBases bases = { { base[0] ? base : client->session_base_url, NULL, NULL }, 1 };
    bool stopped;
    if (client_id[0] && strcmp(client_id, CLOUDMATCH_CLIENT_ID)) {
        snprintf(g_legacy_client_id, sizeof(g_legacy_client_id), "%s", client_id);
        stopped = cloudmatch_stop_as(client, &bases, id, client_id);
        diagnostic_log("CLOUDMATCH", "preflight: stopped an older build's session=%d", stopped);
    } else {
        stopped = cloudmatch_stop_anywhere(&bases, id, headers, header_count);
        diagnostic_log("CLOUDMATCH", "preflight: stopped this console's last session=%d", stopped);
    }
    active_clear();
}

/* RESUME a paused or orphaned session on `base` (OpenNOW desktop's claim).
 * The request repeats the launch identity but not codec, size or bitrate. */
static bool cloudmatch_resume(GfnClient *client, const char *base, const char *app_id, bool full_request)
{
    GfnGame game;
    memset(&game, 0, sizeof(game));
    snprintf(game.app_id, sizeof(game.app_id), "%s", app_id && app_id[0] ? app_id : "0");
    char *full = build_session_body(&game, client->session_device_id);
    json_t *full_root = full ? json_loads(full, 0, NULL) : NULL;
    free(full);
    json_t *source = full_root ? json_object_get(full_root, "sessionRequestData") : NULL;
    json_t *request = full_request && source ? json_deep_copy(source) : json_object();
    if (full_request) source = NULL; /* already copied whole */
    static const char *const keys[] = {
        "appId", "audioMode", "remoteControllersBitmap", "sdrHdrMode", "networkTestSessionId",
        "availableSupportedControllers", "clientVersion", "deviceHashId", "internalTitle",
        "clientPlatformName", "surroundAudioInfo", "clientTimezoneOffset", "clientIdentification",
        "parentSessionId", "streamerVersion", "secureRTSPSupported", "appLaunchMode",
        "enablePersistingInGameSettings"
    };
    for (size_t k = 0; source && k < ARRAY_SIZE(keys); ++k) {
        json_t *value = json_object_get(source, keys[k]);
        if (value) json_object_set(request, keys[k], value);
    }
    json_t *metadata = source ? json_object_get(source, "metaData") : NULL;
    json_t *kept = full_request ? NULL : json_array();
    size_t index;
    json_t *entry;
    json_array_foreach(metadata, index, entry) {
        json_t *key = json_object_get(entry, "key");
        if (json_is_string(key) && !strcmp(json_string_value(key), "clientPhysicalResolution")) continue;
        json_array_append(kept, entry);
    }
    if (kept) json_object_set_new(request, "metaData", kept);
    json_t *body_root = json_pack("{s:i,s:s,s:o,s:n,s:n}", "action", 2, "data", "RESUME",
                                  "sessionRequestData", request, "metaData", "adUpdates");
    json_decref(full_root);
    char *body = body_root ? json_dumps(body_root, JSON_COMPACT) : NULL;
    json_decref(body_root);
    if (!body) return false;

    char url[512];
    snprintf(url, sizeof(url), "%s/v2/session/%s?keyboardLayout=en-US_qwerty&languageCode=en_US",
             base, client->session_id);
    const char *headers[16]; char client_header[80], device_header[80];
    const size_t count = cloudmatch_headers(client, headers, client_header, device_header);
    HttpResponse response;
    const bool sent = http_request("PUT", url, GFN_UA, headers, count, body, 1024 * 1024, &response);
    free(body);
    if (!sent) {
        diagnostic_log("CLOUDMATCH", "resume: no connection");
        return false;
    }
    cloudmatch_log_response("resume-response", &response);
    json_error_t error;
    json_t *root = json_loadb(response.body ? response.body : "", response.size, 0, &error);
    json_t *request_status = root ? json_object_get(root, "requestStatus") : NULL;
    json_t *code = json_is_object(request_status) ? json_object_get(request_status, "statusCode") : NULL;
    const int status_code = json_is_integer(code) ? (int)json_integer_value(code) : -1;
    json_decref(root);
    const long http = response.status;
    http_response_free(&response);
    /* 34 = SESSION_NOT_PAUSED: it is running already; polling finds it. */
    const bool ok = (http >= 200 && http < 300 && status_code == 1) || status_code == 34;
    if (!ok) return false;
    client->resuming_until = (int64_t)time(NULL) + 90;
    client->session_paused = false;
    client->session_state = GFN_SESSION_SETUP;
    client->next_session_poll_at = 0;
    /* The rig answers a RESUME with fresh endpoints; the ones read before it
     * belong to the old connection (beta.19 report: every signalling
     * upgrade after a takeover got 404). The next polls fill them in. */
    memset(client->signaling_url, 0, sizeof(client->signaling_url));
    memset(client->media_ip, 0, sizeof(client->media_ip));
    client->media_port = 0;
    snprintf(client->status, sizeof(client->status), "Resuming your game...");
    return true;
}

/* After a 429 (REQUEST_LIMIT_EXCEEDED), launches wait instead of adding to
 * the pile (beta.17: 36 refused retries in a row). */
static int64_t g_launch_blocked_until;

static void reset_launch_state(GfnClient *client)
{
    memset(client->session_id, 0, sizeof(client->session_id));
    client->queue_best = 0;
    client->queue_step = client->seat_setup_step = -1;
    client->poll_fail_since = 0;
    client->poll_failures = 0;
    client->conflict_found = false;
    client->limit_wait = false;
    client->limit_unclosable = false;
    client->limit_rate = false;
    client->fail_code[0] = '\0';
    client->end_error_code[0] = '\0';
    client->queue_reported = 0;
    client->ads_required = false;
    client->ads_answered = client->ads_pending_count = 0;
    client->session_paused = false;
    client->resuming_until = 0;
    memset(client->signaling_url, 0, sizeof(client->signaling_url));
    memset(client->session_token, 0, sizeof(client->session_token));
    memset(client->server_ip, 0, sizeof(client->server_ip));
    memset(client->media_ip, 0, sizeof(client->media_ip));
    memset(client->session_control_url, 0, sizeof(client->session_control_url));
    client->media_port = 0;
    /* The fixed id (see CLOUDMATCH_CLIENT_ID) and this console's device id. */
    snprintf(client->session_client_id, sizeof(client->session_client_id), "%s", CLOUDMATCH_CLIENT_ID);
    get_session_device_id(client->session_device_id);
}

bool gfn_start_session(GfnClient *client, const GfnGame *game)
{
    const bool quiet = client->limit_quiet;
    client->limit_quiet = false;
    /* This attempt's answer only: launch_failed reads these. */
    client->conflict_found = client->limit_wait = client->limit_rate = false;
    if (steam_link_selected()) return game && steam_link_start_session(client, game);
    if (xcloud_selected()) return game && xcloud_start_session(client, game);
    if (!game) {
        snprintf(client->status, sizeof(client->status), "No game selected; X searches, Y loads library");
        return false;
    }
    if (!gfn_has_session(client)) {
        snprintf(client->status, sizeof(client->status), "Not signed in; press X to sign in again");
        return false;
    }
    if (!refresh_session(client)) {
        /* A timed retry keeps waiting through a moment without Wi-Fi. */
        client->limit_wait = quiet && gfn_has_session(client);
        return false;
    }
    if (gfn_session_active(client)) {
        snprintf(client->status, sizeof(client->status), "Session already active; B stops it");
        return false;
    }
    reset_launch_state(client);
    regions_resolve(client->session_base_url, sizeof(client->session_base_url));
    char *body = build_session_body(game, client->session_device_id);
    if (!body) {
        snprintf(client->status, sizeof(client->status), "Cannot build session request for appId %.40s", game->app_id);
        client->session_state = GFN_SESSION_ERROR;
        return false;
    }
    char url[384];
    snprintf(url, sizeof(url), "%s/v2/session?keyboardLayout=en-US_qwerty&languageCode=en_US",
             client->session_base_url);
    const char *headers[16]; char client_header[80], device_header[80];
    const size_t count = cloudmatch_headers(client, headers, client_header, device_header);
    const int64_t now = (int64_t)time(NULL);
    if (now < g_launch_blocked_until) {
        snprintf(client->status, sizeof(client->status),
                 "NVIDIA is limiting launch attempts. Try again in %lld s.", (long long)(g_launch_blocked_until - now));
        snprintf(client->fail_code, sizeof(client->fail_code), "429");
        client->session_state = GFN_SESSION_ERROR;
        client->limit_wait = client->limit_rate = true;
        free(body);
        return false;
    }
    cloudmatch_stop_remembered(client, headers, count);
    const CloudBases bases = cloud_bases(client, NULL);
    unsigned listed = 0;
    for (unsigned i = 0; i < bases.base_count; ++i)
        listed += cloudmatch_list_active(bases.bases[i], headers, count);
    diagnostic_log("CLOUDMATCH", "preflight listed=%u servers=%u", listed, bases.base_count);

    HttpResponse response;
    bool sent = false;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        sent = http_request("POST", url, GFN_UA, headers, count, body, 1024 * 1024, &response);
        diagnostic_log("CLOUDMATCH", "create attempt=%d transport=%d http=%ld bytes=%lu",
                       attempt, sent ? 1 : 0, sent ? response.status : 0,
                       sent ? (unsigned long)response.size : 0);
        if (sent)
            cloudmatch_log_response("create-response", &response);
        /* A rate limit is not helped by asking again at once. */
        if (!sent || !cloudmatch_status_is_transient(response.status) || response.status == 429 || attempt == 3)
            break;
        http_response_free(&response);
        snprintf(client->status, sizeof(client->status), "CloudMatch HTTP retry %d/3", attempt + 1);
        svcSleepThread((s64)attempt * 2000000000LL);
    }
    free(body);
    if (!sent) {
        snprintf(client->status, sizeof(client->status), "Session create network: %.120s", response.error);
        snprintf(client->fail_code, sizeof(client->fail_code), "network");
        client->session_state = GFN_SESSION_ERROR;
        client->limit_wait = quiet;
        return false;
    }
    if (response.status == 429) {
        g_launch_blocked_until = (int64_t)time(NULL) + 60;
        snprintf(client->status, sizeof(client->status),
                 "NVIDIA is limiting launch attempts for a moment. Try again in a minute.");
        snprintf(client->fail_code, sizeof(client->fail_code), "429");
        client->session_state = GFN_SESSION_ERROR;
        /* The UI waits and tries again by itself: in beta.22 a 429 ended the
         * slot wait with an error (4 launches). */
        client->limit_wait = client->limit_rate = true;
        http_response_free(&response);
        return false;
    }
    if (is_session_limit(&response)) {
        const bool named = capture_conflict(client, &response, game);
        const bool per_device = response.body && strstr(response.body, "PER_DEVICE");
        http_response_free(&response);
        /* Refused "per device" with no session anywhere: the slot is held by
         * something this client can't see or end (hours, on some consoles).
         * A fresh session device id gets a free slot; at most every 10 min. */
        static int64_t rotated_at;
        const int64_t now_s = (int64_t)time(NULL);
        /* Beta.22-23 reports: refusals fell from 78% to 26% of launches
         * after a console got a new id, so swap every 3 min, not 10. */
        if (per_device && !named && !listed && quiet && (!rotated_at || now_s - rotated_at >= 180)) {
            rotated_at = now_s;
            char fresh[40];
            new_session_device_id(fresh);
            diagnostic_log("CLOUDMATCH", "per-device limit during the wait: new session device id for the next try");
        } else if (per_device && !named && !listed && (!rotated_at || now_s - rotated_at >= 180)) {
            rotated_at = now_s;
            char fresh[40];
            new_session_device_id(fresh);
            diagnostic_log("CLOUDMATCH", "per-device limit with nothing listed: new session device id, launching again");
            snprintf(client->status, sizeof(client->status), "Getting a free session slot...");
            client->limit_quiet = quiet;
            const bool started = gfn_start_session(client, game);
            /* An unexpected refusal of the new id (not a limit, capacity or
             * queue answer): go back to the sign-in device id. */
            if (!started && !strcmp(client->fail_code, "http")) {
                remove(SESSION_DEVICE_PATH);
                diagnostic_log("CLOUDMATCH", "new session device id refused; back to the sign-in id");
            }
            return started;
        }
        snprintf(client->fail_code, sizeof(client->fail_code), "limit");
        client->session_state = GFN_SESSION_ERROR;
        /* Asked once per launch; the timed retries after that just wait. */
        client->conflict_found = named && !quiet;
        client->limit_wait = !client->conflict_found;
        snprintf(client->status, sizeof(client->status), client->conflict_found
                 ? "Another GeForce NOW session is running on your account."
                 : "NVIDIA is still closing your other session.");
        return false;
    }
    if (cloudmatch_status_is_transient(response.status)) {
        snprintf(client->status, sizeof(client->status),
                 "Create: CloudMatch HTTP %ld; press A to retry", response.status);
        snprintf(client->fail_code, sizeof(client->fail_code), "busy");
        client->session_state = GFN_SESSION_ERROR;
        http_response_free(&response);
        return false;
    }
    const bool ok = apply_session_response(client, &response, "Create");
    http_response_free(&response);
    return ok;
}

bool gfn_end_conflict(GfnClient *client, const GfnGame *game)
{
    if (!client->conflict.id[0] || !refresh_session(client)) return false;
    const GfnConflict conflict = client->conflict;
    reset_launch_state(client);
    regions_resolve(client->session_base_url, sizeof(client->session_base_url));
    const char *headers[16]; char client_header[80], device_header[80];
    const size_t count = cloudmatch_headers(client, headers, client_header, device_header);
    char rig[160];
    rig_base(conflict.host, rig, sizeof(rig));
    const CloudBases bases = cloud_bases(client, rig);
    snprintf(client->status, sizeof(client->status), "Closing your other session...");
    bool stopped = cloudmatch_stop_anywhere(&bases, conflict.id, headers, count);
    if (!stopped && g_legacy_client_id[0])
        stopped = cloudmatch_stop_as(client, &bases, conflict.id, g_legacy_client_id);
    diagnostic_log("CLOUDMATCH", "end other session: stopped=%d", stopped);
    if (stopped) cloudmatch_wait_clear(&bases, headers, count);
    /* Whatever happened, the next refusal waits instead of asking again. */
    client->limit_quiet = true;
    const bool ok = gfn_start_session(client, game);
    if (!ok && !stopped) client->limit_unclosable = true;
    return ok;
}

bool gfn_claim_conflict(GfnClient *client)
{
    if (xcloud_selected() || steam_link_selected()) return false;
    if (!client->conflict.id[0] || !refresh_session(client)) return false;
    const GfnConflict conflict = client->conflict;
    reset_launch_state(client);
    regions_resolve(client->session_base_url, sizeof(client->session_base_url));
    snprintf(client->session_id, sizeof(client->session_id), "%s", conflict.id);
    const char *headers[16]; char client_header[80], device_header[80];
    const size_t count = cloudmatch_headers(client, headers, client_header, device_header);
    char rig[160];
    rig_base(conflict.host, rig, sizeof(rig));
    const CloudBases bases = cloud_bases(client, rig);
    /* Where does it answer? The rig first, as OpenNOW desktop's claim. */
    for (unsigned i = 0; i < bases.base_count; ++i) {
        char url[512];
        snprintf(url, sizeof(url), "%s/v2/session/%s", bases.bases[i], conflict.id);
        HttpResponse response;
        if (!http_request("GET", url, GFN_UA, headers, count, NULL, 1024 * 1024, &response)) continue;
        const long http = response.status;
        const bool found = http >= 200 && http < 300 && apply_session_response(client, &response, "Claim");
        http_response_free(&response);
        diagnostic_log("CLOUDMATCH", "claim lookup server %u: http=%ld found=%d status=%d", i, http, found,
                       client->session_status);
        if (!found) continue;
        snprintf(client->session_base_url, sizeof(client->session_base_url), "%s", bases.bases[i]);
        client->next_session_poll_at = 0;
        if (client->session_status >= 2 && client->session_status <= 5 &&
            !cloudmatch_resume(client, bases.bases[i], conflict.app_id, false)) {
            snprintf(client->status, sizeof(client->status), "NVIDIA would not hand that session over. "
                     "End it instead, or close it where it runs.");
            snprintf(client->fail_code, sizeof(client->fail_code), "claim");
            client->session_state = GFN_SESSION_ERROR;
            return false;
        }
        if (client->session_state == GFN_SESSION_ERROR) client->session_state = GFN_SESSION_SETUP;
        return true;
    }
    /* Named in the refusal but not found anywhere (beta.19 report: a
     * session queued by beta.18, whose random client id this build can't
     * reach, held the slot for 9+ minutes). It is not gone: it still holds
     * the slot until NVIDIA drops it, so the launch waits for that. */
    if (g_legacy_client_id[0] && cloudmatch_stop_as(client, &bases, conflict.id, g_legacy_client_id))
        diagnostic_log("CLOUDMATCH", "claim: closed it with the older build's client id");
    else
        client->limit_unclosable = true;
    memset(client->session_id, 0, sizeof(client->session_id));
    client->session_state = GFN_SESSION_ERROR;
    client->limit_wait = true;
    snprintf(client->status, sizeof(client->status), "NVIDIA is still closing your other session.");
    snprintf(client->fail_code, sizeof(client->fail_code), "limit");
    diagnostic_log("CLOUDMATCH", "claim: session not reachable; waiting for NVIDIA to free the slot");
    return false;
}

bool gfn_recover_session(GfnClient *client, const GfnGame *game)
{
    if (steam_link_selected()) return steam_link_recover_session(client, game);
    if (xcloud_selected()) return xcloud_recover_session(client);
    const char *app_id = game ? game->app_id : "";
    if (!client->session_id[0]) return false;
    if (!refresh_session(client)) return false;
    const char *headers[16]; char client_header[80], device_header[80];
    const size_t count = cloudmatch_headers(client, headers, client_header, device_header);
    char url[512];
    snprintf(url, sizeof(url), "%s/v2/session/%s", session_control(client), client->session_id);
    HttpResponse response;
    if (!http_request("GET", url, GFN_UA, headers, count, NULL, 1024 * 1024, &response)) {
        snprintf(client->status, sizeof(client->status), "Reconnect: no connection to NVIDIA yet");
        diagnostic_log("CLOUDMATCH", "recover: no connection");
        return false;
    }
    const long http = response.status;
    if (http == 404 || http == 410) {
        http_response_free(&response);
        diagnostic_log("CLOUDMATCH", "recover: session gone http=%ld", http);
        snprintf(client->status, sizeof(client->status), "NVIDIA ended this session. Press A to start the game again.");
        snprintf(client->fail_code, sizeof(client->fail_code), "gone");
        client->session_state = GFN_SESSION_ERROR;
        return false;
    }
    /* NVIDIA may move the session's signalling after a drop: beta.23
     * reconnects kept the first address and got HTTP 503 every time. Read it
     * afresh, and keep the old one only if the answer has none. */
    char old_signaling[sizeof(client->signaling_url)], old_media[sizeof(client->media_ip)];
    const int old_port = client->media_port;
    memcpy(old_signaling, client->signaling_url, sizeof(old_signaling));
    memcpy(old_media, client->media_ip, sizeof(old_media));
    memset(client->signaling_url, 0, sizeof(client->signaling_url));
    memset(client->media_ip, 0, sizeof(client->media_ip));
    client->media_port = 0;
    const bool applied = http >= 200 && http < 300 && apply_session_response(client, &response, "Recover");
    if (!client->signaling_url[0]) {
        memcpy(client->signaling_url, old_signaling, sizeof(old_signaling));
        memcpy(client->media_ip, old_media, sizeof(old_media));
        client->media_port = old_port;
    } else if (strcmp(client->signaling_url, old_signaling)) {
        diagnostic_log("CLOUDMATCH", "recover: signalling moved");
    }
    http_response_free(&response);
    diagnostic_log("CLOUDMATCH", "recover: http=%ld status=%d paused=%d state=%d", http, client->session_status,
                   client->session_paused, client->session_state);
    if (!applied && client->session_state == GFN_SESSION_ERROR && !strcmp(client->fail_code, "ended"))
        return false;
    if (!applied) {
        /* Busy or unknown: keep the session; the next attempt asks again. */
        if (client->session_state == GFN_SESSION_ERROR && !client->session_paused)
            client->session_state = GFN_SESSION_READY;
        return false;
    }
    if (client->session_paused) {
        char rig[160];
        const char *base = rig_base(client->server_ip, rig, sizeof(rig)) ? rig : session_control(client);
        if (!cloudmatch_resume(client, base, app_id, false) && (base == session_control(client) ||
            !cloudmatch_resume(client, session_control(client), app_id, false))) {
            snprintf(client->status, sizeof(client->status), "NVIDIA would not resume this session. "
                     "Press A to start the game again.");
            snprintf(client->fail_code, sizeof(client->fail_code), "resume");
            client->session_state = GFN_SESSION_ERROR;
            return false;
        }
        diagnostic_log("CLOUDMATCH", "recover: resumed the paused session");
    }
    return true;
}

/* Answer queue ads (see note_session_ads). */
static void answer_queue_ads(GfnClient *client, const char **headers, size_t count)
{
    while (client->ads_answered < client->ads_pending_count) {
        const char *id = client->ads_pending[client->ads_answered++];
        char base[256];
        if (!rig_base(client->server_ip, base, sizeof(base)))
            snprintf(base, sizeof(base), "%s", session_control(client));
        char url[512];
        snprintf(url, sizeof(url), "%s/v2/session/%s", base, client->session_id);
        json_t *root = json_pack("{s:i,s:[{s:s,s:i,s:I,s:s}]}", "action", 6, "adUpdates",
                                 "adId", id, "adAction", 5, "clientTimestamp", (json_int_t)time(NULL),
                                 "cancelReason", "error");
        char *body = root ? json_dumps(root, JSON_COMPACT) : NULL;
        json_decref(root);
        if (!body) return;
        HttpResponse response;
        const bool sent = http_request("PUT", url, GFN_UA, headers, count, body, 1024 * 1024, &response);
        free(body);
        if (!sent) {
            diagnostic_log("QUEUEAD", "ad answer: no connection");
            return;
        }
        cloudmatch_log_response("ad-answer", &response);
        if (response.status >= 200 && response.status < 300)
            apply_session_response(client, &response, "Ad");
        http_response_free(&response);
    }
}

void gfn_session_tick(GfnClient *client)
{
    /* A Steam stream is granted at once: nothing to poll. */
    if (steam_link_selected()) return;
    if (xcloud_selected()) {
        xcloud_session_tick(client);
        return;
    }
    if (!gfn_session_active(client) || client->session_state == GFN_SESSION_READY ||
        client->session_state == GFN_SESSION_ERROR) return;
    const int64_t now = (int64_t)time(NULL);
    if (now < client->next_session_poll_at) return;
    client->next_session_poll_at = now + 2;
    char url[512];
    snprintf(url, sizeof(url), "%s/v2/session/%s", session_control(client), client->session_id);
    const char *headers[16]; char client_header[80], device_header[80];
    const size_t count = cloudmatch_headers(client, headers, client_header, device_header);
    HttpResponse response;
    if (!http_request("GET", url, GFN_UA, headers, count, NULL, 1024 * 1024, &response)) {
        snprintf(client->status, sizeof(client->status), "Session poll network: %.124s", response.error);
        diagnostic_log("CLOUDMATCH", "poll transport failure; session retained");
        return;
    }
    const bool transient = cloudmatch_status_is_transient(response.status);
    /* One line per failure streak (beta.16 logged the same 503 102 times,
     * without NVIDIA's reason). */
    if (!transient || !client->poll_failures)
        diagnostic_log("CLOUDMATCH", "poll http=%ld bytes=%lu",
                       response.status, (unsigned long)response.size);
    if (transient) {
        char reason[64] = "";
        json_error_t error;
        json_t *root = json_loadb(response.body ? response.body : "", response.size, 0, &error);
        json_t *request_status = json_is_object(root) ? json_object_get(root, "requestStatus") : NULL;
        json_t *description = json_is_object(request_status) ? json_object_get(request_status, "statusDescription") : NULL;
        if (json_is_string(description)) snprintf(reason, sizeof(reason), "%s", json_string_value(description));
        json_decref(root);
        /* Final answers stop at once (beta.18 kept asking for 30 s after
         * SESSION_REQUEST_IN_QUEUE_ABANDONED). */
        if (strstr(reason, "ABANDONED") || strstr(reason, "ENTITLEMENT") || strstr(reason, "LIMITED_MODE")) {
            cloudmatch_log_response("poll-error", &response);
            apply_session_response(client, &response, "Poll");
            http_response_free(&response);
            return;
        }
        /* The rig is installing a game update: a wait, not an error. */
        if (strstr(reason, "PATCHING")) {
            if (!client->poll_failures) cloudmatch_log_response("poll-patching", &response);
            client->poll_failures = 0;
            snprintf(client->status, sizeof(client->status), "NVIDIA is updating this game on the rig; waiting...");
            client->next_session_poll_at = now + 3;
            http_response_free(&response);
            return;
        }
        if (!client->poll_failures++) {
            client->poll_fail_since = now;
            cloudmatch_log_response("poll-error", &response);
        }
        /* Busy servers: back off 2, 4, 8, then 15 s, and give up after 12
         * errors in a row (~2.5 min, as OpenNOW Vita). Beta.18 gave up at
         * 30 s on the flat 2 s interval. */
        if (client->poll_failures > 12) {
            snprintf(client->status, sizeof(client->status),
                     "NVIDIA's servers stopped answering for this session (HTTP %ld%s%.60s). Press A to try again.",
                     response.status, reason[0] ? ", " : "", reason);
            diagnostic_log("CLOUDMATCH", "poll gave up after %u errors in %llds", client->poll_failures,
                           (long long)(now - client->poll_fail_since));
            snprintf(client->fail_code, sizeof(client->fail_code), "busy");
            client->session_state = GFN_SESSION_ERROR;
            /* An automatically chosen region may be the problem: the retry
             * goes through NVIDIA's own pick. */
            char entry[96];
            provider_base_url(entry, sizeof(entry));
            if (strcmp(client->session_base_url, entry)) regions_avoid_once();
        } else {
            unsigned backoff = 2u << (client->poll_failures - 1 < 3 ? client->poll_failures - 1 : 3);
            if (backoff > 15) backoff = 15;
            snprintf(client->status, sizeof(client->status),
                     "NVIDIA's server is busy (HTTP %ld); still trying...", response.status);
            client->next_session_poll_at = now + backoff;
        }
        http_response_free(&response);
        return;
    }
    if (client->poll_failures)
        diagnostic_log("CLOUDMATCH", "poll recovered after %u errors", client->poll_failures);
    client->poll_failures = 0;
    client->poll_fail_since = 0;
    apply_session_response(client, &response, "Poll");
    http_response_free(&response);
    if (client->ads_answered < client->ads_pending_count && client->session_state != GFN_SESSION_ERROR)
        answer_queue_ads(client, headers, count);
}

/* Renew the login before it runs out, without touching the status line a
 * session screen may be showing. A long game outlasted the login, and the
 * renewal at its end could be refused, which signed the player out. */
bool gfn_keep_login(GfnClient *client)
{
    if (!gfn_has_session(client)) return false;
    char status[sizeof(client->status)];
    memcpy(status, client->status, sizeof(status));
    const bool ok = steam_link_selected() ? true : xcloud_selected() ? xcloud_keep_login(client)
                    : refresh_session(client);
    if (client->auth_state == GFN_AUTH_LOGGED_IN) memcpy(client->status, status, sizeof(status));
    diagnostic_log("AUTH", "background renewal %s; login lasts %llds more", ok ? "done" : "failed",
                   (long long)(client->token_expires_at - (int64_t)time(NULL)));
    return ok;
}

bool gfn_stop_session(GfnClient *client)
{
    if (!client->session_id[0]) {
        client->session_state = GFN_SESSION_IDLE;
        return true;
    }
    if (steam_link_selected()) return steam_link_stop_session(client);
    if (xcloud_selected()) return xcloud_stop_session(client);
    /* After a long game the login may have run out; the stop needs it. */
    if (gfn_has_session(client)) refresh_session(client);
    char url[512];
    snprintf(url, sizeof(url), "%s/v2/session/%s", session_control(client), client->session_id);
    const char *headers[16]; char client_header[80], device_header[80];
    const size_t count = cloudmatch_headers(client, headers, client_header, device_header);
    HttpResponse response;
    if (!http_request("DELETE", url, GFN_UA, headers, count, NULL, 256 * 1024, &response)) {
        snprintf(client->status, sizeof(client->status), "Session stop network: %.124s", response.error);
        return false;
    }
    long status = response.status;
    http_response_free(&response);
    if (status == 404 && client->session_control_url[0] && strcmp(client->session_control_url, client->session_base_url)) {
        snprintf(url, sizeof(url), "%s/v2/session/%s", client->session_base_url, client->session_id);
        if (http_request("DELETE", url, GFN_UA, headers, count, NULL, 256 * 1024, &response)) {
            if (response.status != 404) status = response.status;
            http_response_free(&response);
        }
    }
    /* Not known where it was made: the rig that runs it may still know it
     * (a claimed session, or one a region server handed over). */
    char rig[160];
    if (status == 404 && rig_base(client->server_ip, rig, sizeof(rig)) && strcmp(rig, client->session_base_url)) {
        snprintf(url, sizeof(url), "%s/v2/session/%s", rig, client->session_id);
        if (http_request("DELETE", url, GFN_UA, headers, count, NULL, 256 * 1024, &response)) {
            diagnostic_log("CLOUDMATCH", "stop: 404 on its server, rig says http=%ld", response.status);
            if (response.status != 404) status = response.status;
            http_response_free(&response);
        }
    }
    const bool ok = (status >= 200 && status < 300) || status == 404;
    diagnostic_log("CLOUDMATCH", "stop session: http=%ld (control %s)", status,
                   client->session_control_url[0] ? "server" : "base");
    snprintf(client->status, sizeof(client->status), ok ? "Cloud session stopped" : "Session stop HTTP %ld", status);
    if (ok) {
        memset(client->session_id, 0, sizeof(client->session_id));
        client->session_state = GFN_SESSION_IDLE;
        active_clear();
    }
    return ok;
}

/* ---- Resume after a crash ---------------------------------------------------- */

void gfn_active_save(const GfnClient *client, const GfnGame *game)
{
    /* Xbox sessions are not resumed after a crash (yet). */
    if (!client->session_id[0] || !game || xcloud_selected() || steam_link_selected()) return;
    json_t *root = json_pack("{s:s,s:s,s:s,s:s,s:I,s:{s:s,s:s,s:s,s:s}}",
                             "session_id", client->session_id,
                             "client_id", client->session_client_id,
                             "device_id", client->session_device_id,
                             "base_url", client->session_base_url,
                             "saved_at", (json_int_t)time(NULL),
                             "game", "title", game->title, "id", game->app_id,
                             "store", game->store, "image", game->image_url);
    if (root) json_dump_file(root, ACTIVE_SESSION_PATH, JSON_COMPACT);
    json_decref(root);
}

static void active_clear(void) { remove(ACTIVE_SESSION_PATH); }

bool gfn_login_saved(void)
{
    struct stat st;
    return stat(SESSION_PATH, &st) == 0;
}

bool gfn_active_exists(void)
{
    struct stat st;
    return stat(ACTIVE_SESSION_PATH, &st) == 0;
}

bool gfn_resume_check(GfnClient *client)
{
    client->resume_found = false;
    if (xcloud_selected() || steam_link_selected()) {
        active_clear();
        return false;
    }
    json_error_t error;
    json_t *root = json_load_file(ACTIVE_SESSION_PATH, 0, &error);
    if (!json_is_object(root)) {
        json_decref(root);
        active_clear();
        return false;
    }
    /* NVIDIA ends an abandoned rig after a while; don't bother after a day. */
    json_t *saved = json_object_get(root, "saved_at");
    const int64_t age = (int64_t)time(NULL) - (json_is_integer(saved) ? json_integer_value(saved) : 0);
    if (age > 24 * 3600 || !gfn_has_session(client) || !refresh_session(client)) {
        json_decref(root);
        if (age > 24 * 3600) active_clear();
        return false;
    }
    memset(&client->resume_game, 0, sizeof(client->resume_game));
    copy_json_string(client->session_id, sizeof(client->session_id), root, "session_id");
    copy_json_string(client->session_client_id, sizeof(client->session_client_id), root, "client_id");
    copy_json_string(client->session_device_id, sizeof(client->session_device_id), root, "device_id");
    copy_json_string(client->session_base_url, sizeof(client->session_base_url), root, "base_url");
    json_t *game = json_object_get(root, "game");
    if (json_is_object(game)) {
        copy_json_string(client->resume_game.title, sizeof(client->resume_game.title), game, "title");
        copy_json_string(client->resume_game.app_id, sizeof(client->resume_game.app_id), game, "id");
        copy_json_string(client->resume_game.store, sizeof(client->resume_game.store), game, "store");
        copy_json_string(client->resume_game.image_url, sizeof(client->resume_game.image_url), game, "image");
    }
    json_decref(root);
    if (!client->session_id[0] || !client->session_base_url[0]) {
        active_clear();
        memset(client->session_id, 0, sizeof(client->session_id));
        return false;
    }

    client->queue_best = 0;
    client->queue_step = client->seat_setup_step = -1;
    client->resuming_until = 0;
    char url[512];
    snprintf(url, sizeof(url), "%s/v2/session/%s", client->session_base_url, client->session_id);
    const char *headers[16]; char client_header[80], device_header[80];
    const size_t count = cloudmatch_headers(client, headers, client_header, device_header);
    HttpResponse response;
    const bool sent = http_request("GET", url, GFN_UA, headers, count, NULL, 1024 * 1024, &response);
    bool alive = sent && response.status >= 200 && response.status < 300 &&
                 apply_session_response(client, &response, "Resume") &&
                 client->session_state != GFN_SESSION_ERROR;
    const long http = sent ? response.status : 0;
    http_response_free(&response);
    /* Paused (the console went away mid-game): RESUME brings it back. */
    if (!alive && client->session_paused) {
        char rig[160];
        const char *base = rig_base(client->server_ip, rig, sizeof(rig)) ? rig : client->session_base_url;
        alive = cloudmatch_resume(client, base, client->resume_game.app_id, false);
    }
    diagnostic_log("CLOUDMATCH", "resume check http=%ld alive=%d state=%d paused=%d",
                   http, alive, client->session_state, client->session_paused);
    if (!alive) {
        /* Gone: forget it quietly and start clean. */
        active_clear();
        memset(client->session_id, 0, sizeof(client->session_id));
        client->session_state = GFN_SESSION_IDLE;
        snprintf(client->status, sizeof(client->status), "Library ready");
        return false;
    }
    client->resume_found = true;
    snprintf(client->status, sizeof(client->status), "Your game is still running");
    return true;
}

bool gfn_session_active(const GfnClient *client)
{
    return client->session_id[0] != '\0' && client->session_state != GFN_SESSION_IDLE;
}

void gfn_client_switch_service(GfnClient *client)
{
    memset(client->access_token, 0, sizeof(client->access_token));
    memset(client->refresh_token, 0, sizeof(client->refresh_token));
    memset(client->id_token, 0, sizeof(client->id_token));
    memset(client->client_token, 0, sizeof(client->client_token));
    memset(client->device_code, 0, sizeof(client->device_code));
    client->user_id[0] = client->catalog_vpc[0] = '\0';
    client->token_expires_at = client->client_token_expires_at = 0;
    client->game_count = client->catalog_total = 0;
    client->library_saved_at = 0;
    if (steam_link_selected() ? steam_link_load_login(client)
        : xcloud_selected() ? xcloud_load_login(client) : load_session(client)) {
        client->auth_state = GFN_AUTH_LOGGED_IN;
        gfn_library_load(client);
    } else {
        client->auth_state = GFN_AUTH_LOGGED_OUT;
        snprintf(client->status, sizeof(client->status), steam_link_selected() ? "Press X to pair with your PC"
                 : xcloud_selected() ? "Press X to sign in with Microsoft" : "Press X to sign in with NVIDIA");
    }
    diagnostic_log("AUTH", "service now %s, %s",
                   steam_link_selected() ? "Steam Link" : xcloud_selected() ? "Xbox" : "GeForce NOW",
                   client->auth_state == GFN_AUTH_LOGGED_IN ? "signed in" : "signed out");
}

void gfn_sign_out(GfnClient *client)
{
    client->catalog_vpc[0] = '\0';
    if (steam_link_selected()) {
        remove(STEAM_LIBRARY_PATH);
        client->library_saved_at = 0;
        client->game_count = client->catalog_total = 0;
        if (steam_link_sign_out(client)) {
            /* Another paired PC took its place. */
            client->auth_state = GFN_AUTH_LOGGED_IN;
            gfn_fetch_library(client);
            snprintf(client->status, sizeof(client->status), "PC forgotten; now using %.60s", steam_link_host_name());
            return;
        }
    } else if (xcloud_selected()) {
        xcloud_sign_out();
    } else {
        remove(SESSION_PATH);
        provider_set_active(NULL);
        remove(LIBRARY_CACHE_PATH);
    }
    client->library_saved_at = 0;
    memset(client->access_token, 0, sizeof(client->access_token));
    memset(client->refresh_token, 0, sizeof(client->refresh_token));
    memset(client->id_token, 0, sizeof(client->id_token));
    memset(client->client_token, 0, sizeof(client->client_token));
    memset(client->device_code, 0, sizeof(client->device_code));
    client->token_expires_at = client->client_token_expires_at = 0;
    client->game_count = client->catalog_total = 0;
    client->auth_state = GFN_AUTH_LOGGED_OUT;
    snprintf(client->status, sizeof(client->status), "Signed out; saved login removed");
}
