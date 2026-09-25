#include "gfn_client.h"
#include "app_paths.h"
#include "http_client.h"
#include "diagnostic.h"
#include "stream_profile.h"

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
#define DEVICE_PATH DATA_DIR "/device-id.txt"

static const char *DEVICE_CLIENT_ID = "q61ddeJrVt7O90Nl-P-N7I36yctih4Ml6FyXLrb6j-U";
static const char *NVIDIA_IDP = "PDiAhv2kJTFeQ7WOPqiQ2tRZ7lGhR2X11dXvM4TZSxg";
static const char *DEVICE_UA = "Mozilla/5.0 (X11; Linux x86_64; Steam Deck) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36";
static const char *GFN_UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36 NVIDIACEFClient/HEAD/debb5919f6 GFN-PC/2.0.80.173";
/* Requests are serialized on the app thread; avoid an 8 KiB caller frame during TLS. */
static char g_authorization_header[8300];

static bool cloudmatch_status_is_transient(long status)
{
    return status == 429 || status == 502 || status == 503 || status == 504;
}

static void generate_uuid(char output[40])
{
    unsigned char bytes[16];
    for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = (unsigned char)rand();
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
    file = fopen(DEVICE_PATH, "w");
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
    diagnostic_log("AUTH", "client-token http=%ld token=%s", response.status,
                   client->client_token[0] ? "present" : "missing");
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

static bool request_tokens(GfnClient *client, const char *form, bool polling)
{
    const char *headers[] = {
        "Origin: https://play.geforcenow.com",
        "Referer: https://play.geforcenow.com/",
        "Accept: application/json, text/plain, */*",
        "Content-Type: application/x-www-form-urlencoded; charset=UTF-8"
    };
    HttpResponse response;
    if (!http_request("POST", "https://login.nvidia.com/token", DEVICE_UA,
                      headers, ARRAY_SIZE(headers), form, 128 * 1024, &response)) {
        snprintf(client->status, sizeof(client->status), "Token request: %.130s", response.error);
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
        if (rotated_client_token) client->client_token_expires_at = now + lifetime;
        json_decref(root);
        http_response_free(&response);
        client->auth_state = GFN_AUTH_LOGGED_IN;
        hydrate_session_identity(client);
        const bool saved = save_session(client);
        snprintf(client->status, sizeof(client->status), "Signed in; session %s", saved ? "saved to SD" : "save failed");
        return true;
    }

    const char *code = "";
    if (root) {
        json_t *error_value = json_object_get(root, "error");
        if (json_is_string(error_value)) code = json_string_value(error_value);
    }
    if (polling && strcmp(code, "authorization_pending") == 0) {
        snprintf(client->status, sizeof(client->status), "Waiting for browser sign-in...");
    } else if (polling && strcmp(code, "slow_down") == 0) {
        client->poll_interval += 5;
        snprintf(client->status, sizeof(client->status), "NVIDIA asked to slow polling");
    } else {
        snprintf(client->status, sizeof(client->status), "Token HTTP %ld: %.80s", response.status, code);
        client->auth_state = GFN_AUTH_ERROR;
    }
    if (root) json_decref(root);
    http_response_free(&response);
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
    if (client->client_token[0] && client->user_id[0]) {
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
        diagnostic_log("AUTH", "client-token refresh failed; trying OAuth refresh");
    }
    if (!client->refresh_token[0]) {
        client->auth_state = GFN_AUTH_ERROR;
        snprintf(client->status, sizeof(client->status), "Login expired without refresh token; X to sign in again");
        return false;
    }
    char *encoded = http_url_encode(client->refresh_token);
    if (!encoded) {
        snprintf(client->status, sizeof(client->status), "Login refresh: not enough memory to encode token");
        return false;
    }
    const size_t length = strlen(encoded) + strlen(DEVICE_CLIENT_ID) + 96;
    char *form = malloc(length);
    if (!form) {
        free(encoded);
        snprintf(client->status, sizeof(client->status), "Login refresh: not enough memory for request");
        return false;
    }
    snprintf(form, length, "grant_type=refresh_token&refresh_token=%s&client_id=%s", encoded, DEVICE_CLIENT_ID);
    free(encoded);
    diagnostic_log("AUTH", "refresh method=oauth-refresh-token");
    const bool ok = request_tokens(client, form, false);
    free(form);
    return ok;
}

void gfn_client_init(GfnClient *client)
{
    memset(client, 0, sizeof(*client));
    srand((unsigned)(svcGetSystemTick() ^ osGetTime()));
    if (load_session(client)) {
        client->auth_state = GFN_AUTH_LOGGED_IN;
        snprintf(client->status, sizeof(client->status), "Saved NVIDIA session loaded");
    } else {
        client->auth_state = GFN_AUTH_LOGGED_OUT;
        snprintf(client->status, sizeof(client->status), "Press X to sign in with NVIDIA");
    }
}

bool gfn_begin_login(GfnClient *client)
{
    client->catalog_vpc[0] = '\0';
    char device_id[40];
    get_device_id(device_id);
    char form[768];
    snprintf(form, sizeof(form),
             "client_id=%s&scope=openid%%20consent%%20email%%20tk_client%%20age&device_id=%s&display_name=Kasumi-3DS&idp_id=%s",
             DEVICE_CLIENT_ID, device_id, NVIDIA_IDP);
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
        snprintf(client->status, sizeof(client->status), "Login TLS/network: %.128s", response.error);
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
    snprintf(client->status, sizeof(client->status), "Open URL on phone/PC and enter code");
    return true;
}

void gfn_tick(GfnClient *client)
{
    if (client->auth_state != GFN_AUTH_WAITING) return;
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
    HttpResponse server_info;
    if (client->catalog_vpc[0] && (int64_t)time(NULL) < client->catalog_vpc_expires_at) {
        snprintf(vpc_id, sizeof(vpc_id), "%s", client->catalog_vpc);
    } else if (http_request("GET", "https://prod.cloudmatchbeta.nvidiagrid.net/v2/serverInfo", GFN_UA,
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
    #define APP_FIELDS "items{id title images{GAME_BOX_ART KEY_ART TV_BANNER} " \
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
    } else {
        snprintf(client->status, sizeof(client->status), "Loaded %lu owned games (server total %lu)",
                 (unsigned long)client->game_count, (unsigned long)client->catalog_total);
    }
    return true;
}

#define LIBRARY_CACHE_PATH APP_DATA_DIR "/library.json"

/* The owned library is kept on the SD card so the next start shows it at
 * once; Refresh fetches it again. It holds titles, IDs and art URLs only. */
static void library_save(const GfnClient *client)
{
    json_t *games = json_array();
    for (size_t i = 0; i < client->game_count; ++i) {
        const GfnGame *g = &client->games[i];
        json_t *variants = json_array();
        for (unsigned v = 0; v < g->variant_count; ++v)
            json_array_append_new(variants, json_pack("[s,s]", g->variants[v].id, g->variants[v].store));
        json_array_append_new(games, json_pack("{s:s,s:s,s:s,s:s,s:o,s:i}", "title", g->title,
                                               "id", g->app_id, "store", g->store,
                                               "image", g->image_url, "variants", variants,
                                               "selected", (int)g->variant_selected));
    }
    json_t *root = json_pack("{s:I,s:o}", "saved_at", (json_int_t)client->library_saved_at,
                             "games", games);
    if (root) json_dump_file(root, LIBRARY_CACHE_PATH, JSON_COMPACT);
    json_decref(root);
}

bool gfn_library_load(GfnClient *client)
{
    json_error_t error;
    json_t *root = json_load_file(LIBRARY_CACHE_PATH, 0, &error);
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
    snprintf(client->status, sizeof(client->status), "Library: %lu games (Y refreshes)",
             (unsigned long)client->game_count);
    return true;
}

/* Latency: the second of two small requests to NVIDIA's session service,
 * so TLS setup is not counted. Throughput: a 768 KiB ranged download from
 * NVIDIA's CDN after a warm-up request on the same connection. */
bool gfn_connection_test(GfnClient *client)
{
    static const char *const info_url = "https://prod.cloudmatchbeta.nvidiagrid.net/v2/serverInfo";
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
    if (!fetch_catalog(client, NULL, true)) return false;
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
     * default, like OpenNOW. Settings > Encoder filter brings it back. */
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
    json_object_set_new(monitor, "framesPerSecond", json_integer(30));
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

static bool apply_session_response(GfnClient *client, HttpResponse *response, const char *operation)
{
    json_error_t error;
    json_t *root = json_loadb(response->body ? response->body : "", response->size, 0, &error);
    if (!root) {
        snprintf(client->status, sizeof(client->status), "%s: invalid JSON", operation);
        client->session_state = GFN_SESSION_ERROR;
        return false;
    }
    json_t *request_status = json_object_get(root, "requestStatus");
    json_t *code_value = json_is_object(request_status) ? json_object_get(request_status, "statusCode") : NULL;
    const int status_code = json_is_integer(code_value) ? (int)json_integer_value(code_value) : -1;
    if (response->status < 200 || response->status >= 300 || status_code != 1) {
        snprintf(client->status, sizeof(client->status), "%s: CloudMatch HTTP %ld code %d",
                 operation, response->status, status_code);
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
        client->session_state = GFN_SESSION_ERROR;
        json_decref(root);
        return false;
    }
    flexible_json_text(client->session_id, sizeof(client->session_id), json_object_get(session, "sessionId"));
    if (!client->session_id[0]) {
        snprintf(client->status, sizeof(client->status), "%s: CloudMatch response has no session ID", operation);
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
    if (queued && client->queue_best <= 0) {
        client->queue_step = client->seat_setup_step;
        client->queue_best = reported;
    } else if (queued && client->seat_setup_step != client->queue_step) {
        queued = false;
    } else if (queued && reported < client->queue_best) {
        client->queue_best = reported;
    }
    client->queue_position = queued ? client->queue_best : 0;
    diagnostic_log("CLOUDMATCH", "queue shown=%d reported=%d session=%d seat=%d root=%d step=%d queueStep=%d best=%d status=%d",
                   client->queue_position, reported, client->queue_session_position,
                   client->queue_seat_position, client->queue_root_position,
                   client->seat_setup_step, client->queue_step, client->queue_best,
                   client->session_status);
    parse_session_network(client, session);
    const bool ad_required = json_is_true(json_object_get(session, "sessionAdsRequired"));
    if (client->session_status == 4) {
        client->session_state = GFN_SESSION_ERROR;
        snprintf(client->status, sizeof(client->status), "CloudMatch session ended (status 4); B clears it");
    } else if ((client->session_status == 2 || client->session_status == 3) && client->signaling_url[0]) {
        client->session_state = GFN_SESSION_READY;
        snprintf(client->status, sizeof(client->status), "Session ready; signaling endpoint received");
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
        "nv-client-type: NATIVE", "nv-client-version: 2.0.80.173",
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

/* CloudMatch permits only one active stream for this device identity.  A HOME
 * menu close, power loss, or crash can skip our normal DELETE and leave a rig
 * occupying that slot.  Current OpenNOW Vita performs this bounded cleanup
 * before each create request as well. */
static void cloudmatch_cleanup_stale_sessions(GfnClient *client,
                                              const char **headers,
                                              size_t header_count)
{
    char list_url[384];
    snprintf(list_url, sizeof(list_url), "%s/v2/session", client->session_base_url);
    HttpResponse list_response;
    if (!http_request("GET", list_url, GFN_UA, headers, header_count,
                      NULL, 1024 * 1024, &list_response)) {
        diagnostic_log("CLOUDMATCH", "preflight list transport failure");
        return;
    }
    cloudmatch_log_response("preflight-list", &list_response);
    if (list_response.status < 200 || list_response.status >= 300) {
        http_response_free(&list_response);
        return;
    }

    json_error_t error;
    json_t *root = json_loadb(list_response.body ? list_response.body : "",
                              list_response.size, 0, &error);
    json_t *sessions = root ? json_object_get(root, "sessions") : NULL;
    unsigned active = 0, stopped = 0;
    size_t index;
    json_t *session;
    json_array_foreach(sessions, index, session) {
        char session_id[160] = "";
        flexible_json_text(session_id, sizeof(session_id),
                           json_object_get(session, "sessionId"));
        const int status = parse_session_status(json_object_get(session, "status"));
        if (!session_id[0] || status < 0 || status > 3)
            continue;
        active++;
        char delete_url[512];
        snprintf(delete_url, sizeof(delete_url), "%s/v2/session/%s",
                 client->session_base_url, session_id);
        HttpResponse delete_response;
        if (http_request("DELETE", delete_url, GFN_UA, headers, header_count,
                         NULL, 256 * 1024, &delete_response)) {
            diagnostic_log("CLOUDMATCH", "preflight-delete index=%u status=%d http=%ld",
                           active, status, delete_response.status);
            if ((delete_response.status >= 200 && delete_response.status < 300) ||
                delete_response.status == 404)
                stopped++;
            http_response_free(&delete_response);
        }
    }
    diagnostic_log("CLOUDMATCH", "preflight active=%u stopped=%u", active, stopped);
    if (root) json_decref(root);
    http_response_free(&list_response);
    if (stopped)
        svcSleepThread(3000000000LL);
}

bool gfn_start_session(GfnClient *client, const GfnGame *game)
{
    if (!game) {
        snprintf(client->status, sizeof(client->status), "No game selected; X searches, Y loads library");
        return false;
    }
    if (!gfn_has_session(client)) {
        snprintf(client->status, sizeof(client->status), "Not signed in; press X to sign in again");
        return false;
    }
    if (!refresh_session(client)) return false;
    if (gfn_session_active(client)) {
        snprintf(client->status, sizeof(client->status), "Session already active; B stops it");
        return false;
    }
    memset(client->session_id, 0, sizeof(client->session_id));
    client->queue_best = 0;
    client->queue_step = client->seat_setup_step = -1;
    memset(client->signaling_url, 0, sizeof(client->signaling_url));
    memset(client->session_token, 0, sizeof(client->session_token));
    memset(client->server_ip, 0, sizeof(client->server_ip));
    memset(client->media_ip, 0, sizeof(client->media_ip));
    client->media_port = 0;
    generate_uuid(client->session_client_id);
    get_device_id(client->session_device_id);
    snprintf(client->session_base_url, sizeof(client->session_base_url),
             "https://prod.cloudmatchbeta.nvidiagrid.net");
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
    cloudmatch_cleanup_stale_sessions(client, headers, count);
    HttpResponse response;
    bool sent = false;
    int attempt = 0;
    for (attempt = 1; attempt <= 3; ++attempt) {
        sent = http_request("POST", url, GFN_UA, headers, count, body, 1024 * 1024, &response);
        diagnostic_log("CLOUDMATCH", "create attempt=%d transport=%d http=%ld bytes=%lu",
                       attempt, sent ? 1 : 0, sent ? response.status : 0,
                       sent ? (unsigned long)response.size : 0);
        if (sent)
            cloudmatch_log_response("create-response", &response);
        if (!sent || !cloudmatch_status_is_transient(response.status) || attempt == 3)
            break;
        http_response_free(&response);
        snprintf(client->status, sizeof(client->status),
                 "CloudMatch HTTP retry %d/3", attempt + 1);
        svcSleepThread((s64)attempt * 2000000000LL);
    }
    free(body);
    if (!sent) {
        snprintf(client->status, sizeof(client->status), "Session create network: %.120s", response.error);
        client->session_state = GFN_SESSION_ERROR;
        return false;
    }
    if (cloudmatch_status_is_transient(response.status)) {
        snprintf(client->status, sizeof(client->status),
                 "Create: CloudMatch HTTP %ld; press A to retry", response.status);
        client->session_state = GFN_SESSION_ERROR;
        http_response_free(&response);
        return false;
    }
    const bool ok = apply_session_response(client, &response, "Create");
    http_response_free(&response);
    return ok;
}

void gfn_session_tick(GfnClient *client)
{
    if (!gfn_session_active(client) || client->session_state == GFN_SESSION_READY ||
        client->session_state == GFN_SESSION_ERROR) return;
    const int64_t now = (int64_t)time(NULL);
    if (now < client->next_session_poll_at) return;
    client->next_session_poll_at = now + 2;
    char url[512];
    snprintf(url, sizeof(url), "%s/v2/session/%s", client->session_base_url, client->session_id);
    const char *headers[16]; char client_header[80], device_header[80];
    const size_t count = cloudmatch_headers(client, headers, client_header, device_header);
    HttpResponse response;
    if (!http_request("GET", url, GFN_UA, headers, count, NULL, 1024 * 1024, &response)) {
        snprintf(client->status, sizeof(client->status), "Session poll network: %.124s", response.error);
        diagnostic_log("CLOUDMATCH", "poll transport failure; session retained");
        return;
    }
    diagnostic_log("CLOUDMATCH", "poll http=%ld bytes=%lu",
                   response.status, (unsigned long)response.size);
    if (cloudmatch_status_is_transient(response.status)) {
        snprintf(client->status, sizeof(client->status),
                 "CloudMatch poll HTTP %ld; retrying", response.status);
        client->next_session_poll_at = now + 2;
        http_response_free(&response);
        return;
    }
    apply_session_response(client, &response, "Poll");
    http_response_free(&response);
}

bool gfn_stop_session(GfnClient *client)
{
    if (!client->session_id[0]) {
        client->session_state = GFN_SESSION_IDLE;
        return true;
    }
    char url[512];
    snprintf(url, sizeof(url), "%s/v2/session/%s", client->session_base_url, client->session_id);
    const char *headers[16]; char client_header[80], device_header[80];
    const size_t count = cloudmatch_headers(client, headers, client_header, device_header);
    HttpResponse response;
    if (!http_request("DELETE", url, GFN_UA, headers, count, NULL, 256 * 1024, &response)) {
        snprintf(client->status, sizeof(client->status), "Session stop network: %.124s", response.error);
        return false;
    }
    const bool ok = (response.status >= 200 && response.status < 300) || response.status == 404;
    snprintf(client->status, sizeof(client->status), ok ? "Cloud session stopped" : "Session stop HTTP %ld", response.status);
    http_response_free(&response);
    if (ok) {
        memset(client->session_id, 0, sizeof(client->session_id));
        client->session_state = GFN_SESSION_IDLE;
        active_clear();
    }
    return ok;
}

/* ---- Resume after a crash ---------------------------------------------------- */

#define ACTIVE_SESSION_PATH APP_DATA_DIR "/active-session.json"

void gfn_active_save(const GfnClient *client, const GfnGame *game)
{
    if (!client->session_id[0] || !game) return;
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

bool gfn_active_exists(void)
{
    struct stat st;
    return stat(ACTIVE_SESSION_PATH, &st) == 0;
}

bool gfn_resume_check(GfnClient *client)
{
    client->resume_found = false;
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
    char url[512];
    snprintf(url, sizeof(url), "%s/v2/session/%s", client->session_base_url, client->session_id);
    const char *headers[16]; char client_header[80], device_header[80];
    const size_t count = cloudmatch_headers(client, headers, client_header, device_header);
    HttpResponse response;
    const bool sent = http_request("GET", url, GFN_UA, headers, count, NULL, 1024 * 1024, &response);
    const bool alive = sent && response.status >= 200 && response.status < 300 &&
                       apply_session_response(client, &response, "Resume") &&
                       client->session_state != GFN_SESSION_ERROR;
    diagnostic_log("CLOUDMATCH", "resume check http=%ld alive=%d state=%d",
                   sent ? response.status : 0, alive, client->session_state);
    http_response_free(&response);
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

void gfn_sign_out(GfnClient *client)
{
    remove(SESSION_PATH);
    remove(LIBRARY_CACHE_PATH);
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
