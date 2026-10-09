#include "provider.h"

#include <3ds.h>
#include <jansson.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>

#include "app_paths.h"
#include "diagnostic.h"
#include "http_client.h"

#define PROVIDERS_PATH APP_DATA_DIR "/providers.json"
#define SERVICE_URLS "https://pcs.geforcenow.com/v1/serviceUrls"

static LightLock g_lock = 1;
static GfnProvider g_list[PROVIDER_MAX];
static unsigned g_count;
static char g_recommended[12];
/* From the provider list: the country NVIDIA sees, and whether the
 * recommendation is ours because NVIDIA names none (partner only). */
static char g_country[4];

/* NVIDIA's two-letter country code, upper case; anything else is dropped
 * (a launch error showed three unreadable boxes where the country goes). */
static void country_code(char out[4], const char *value)
{
    out[0] = '\0';
    if (!value || strlen(value) != 2 || !isalpha((unsigned char)value[0]) || !isalpha((unsigned char)value[1]))
        return;
    out[0] = (char)toupper((unsigned char)value[0]);
    out[1] = (char)toupper((unsigned char)value[1]);
    out[2] = '\0';
}
static bool g_partner_only;
static GfnProvider g_active;
static bool g_active_set;

void provider_nvidia(GfnProvider *out)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->code, sizeof(out->code), "%s", PROVIDER_NVIDIA);
    snprintf(out->name, sizeof(out->name), "NVIDIA");
    snprintf(out->idp, sizeof(out->idp), "%s", PROVIDER_NVIDIA_IDP);
    snprintf(out->url, sizeof(out->url), "%s", PROVIDER_NVIDIA_URL);
}

void provider_xbox(GfnProvider *out)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->code, sizeof(out->code), "%s", PROVIDER_XBOX);
    snprintf(out->name, sizeof(out->name), "Xbox Cloud Gaming (beta)");
    snprintf(out->idp, sizeof(out->idp), "xbox");
    snprintf(out->url, sizeof(out->url), "%s", PROVIDER_XBOX_URL);
}

/* Only NVIDIA-hosted https session services, an idp of plain characters:
 * the values come from the network and go into URLs and forms. */
static bool valid(const GfnProvider *p)
{
    if (!p->code[0] || !p->idp[0] || strncmp(p->url, "https://", 8)) return false;
    const char *host = p->url + 8;
    const size_t host_len = strcspn(host, "/:?#");
    if (host_len < 16 || host[host_len] != '\0') return false;
    if (strncasecmp(host + host_len - 15, ".nvidiagrid.net", 15)) return false;
    for (const char *c = p->idp; *c; ++c)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
              *c == '-' || *c == '_'))
            return false;
    return true;
}

/* NVIDIA's own name for a provider, before the region Kasumi adds ("au"
 * for "au (KDDI, Japan)"). */
static bool name_matches(const char *shown, const char *wanted)
{
    const size_t n = strlen(wanted);
    return !strcasecmp(shown, wanted) || (!strncasecmp(shown, wanted, n) && !strncmp(shown + n, " (", 2));
}

/* Fill `out` from one serviceUrls endpoint entry. */
static bool from_json(json_t *entry, GfnProvider *out)
{
    memset(out, 0, sizeof(*out));
    const char *code = json_string_value(json_object_get(entry, "loginProviderCode"));
    const char *name = json_string_value(json_object_get(entry, "loginProviderDisplayName"));
    const char *idp = json_string_value(json_object_get(entry, "idpId"));
    const char *url = json_string_value(json_object_get(entry, "streamingServiceUrl"));
    if (!code || !idp || !url || strlen(code) >= sizeof(out->code) || strlen(idp) >= sizeof(out->idp) ||
        strlen(url) >= sizeof(out->url))
        return false;
    snprintf(out->code, sizeof(out->code), "%s", code);
    /* Same display fix as OpenNOW: BPC's own name is "Brothers Pictures".
     * Where a partner serves: KDDI's "au" read as Australia to a player
     * there (beta.29 report CKWQJZ), whose provider is Cloud.GG. */
    static const struct { const char *code, *name; } names[] = {
        { "BPC", "bro.game" }, { "KDD", "au (KDDI, Japan)" }, { "PNT", "Cloud.GG (Australia)" },
        { "TWM", "Taiwan Mobile (Taiwan)" }, { "STR", "StarHub (Singapore)" }, { "GKR", "GFN Korea" },
        { "RAN", "rain (South Africa)" }, { "DIG", "Digevo (Latin America)" },
    };
    const char *shown = name && name[0] ? name : code;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (!strcmp(code, names[i].code)) shown = names[i].name;
    snprintf(out->name, sizeof(out->name), "%s", shown);
    snprintf(out->idp, sizeof(out->idp), "%s", idp);
    snprintf(out->url, sizeof(out->url), "%s", url);
    size_t n = strlen(out->url);
    while (n > 8 && out->url[n - 1] == '/') out->url[--n] = '\0';
    return valid(out);
}

static void save(void)
{
    json_t *list = json_array();
    LightLock_Lock(&g_lock);
    for (unsigned i = 0; i < g_count; ++i)
        json_array_append_new(list, json_pack("{s:s,s:s,s:s,s:s}", "code", g_list[i].code, "name", g_list[i].name,
                                              "idp", g_list[i].idp, "url", g_list[i].url));
    json_t *root = json_pack("{s:o,s:s,s:s,s:b}", "providers", list, "recommended", g_recommended,
                             "country", g_country, "partner_only", g_partner_only);
    LightLock_Unlock(&g_lock);
    if (root) json_dump_file(root, PROVIDERS_PATH, JSON_COMPACT);
    json_decref(root);
}

void providers_load(void)
{
    /* The lock is statically initialised: the saved login may already have
     * set the active provider. */
    json_error_t error;
    json_t *root = json_load_file(PROVIDERS_PATH, 0, &error);
    json_t *list = json_is_object(root) ? json_object_get(root, "providers") : NULL;
    size_t index;
    json_t *item;
    g_count = 0;
    json_array_foreach(list, index, item) {
        if (g_count >= PROVIDER_MAX) break;
        GfnProvider p;
        memset(&p, 0, sizeof(p));
        const char *code = json_string_value(json_object_get(item, "code"));
        const char *name = json_string_value(json_object_get(item, "name"));
        const char *idp = json_string_value(json_object_get(item, "idp"));
        const char *url = json_string_value(json_object_get(item, "url"));
        if (!code || !name || !idp || !url) continue;
        snprintf(p.code, sizeof(p.code), "%s", code);
        snprintf(p.name, sizeof(p.name), "%s", name);
        snprintf(p.idp, sizeof(p.idp), "%s", idp);
        snprintf(p.url, sizeof(p.url), "%s", url);
        if (valid(&p)) g_list[g_count++] = p;
    }
    const char *recommended = json_is_object(root) ? json_string_value(json_object_get(root, "recommended")) : NULL;
    snprintf(g_recommended, sizeof(g_recommended), "%s", recommended ? recommended : "");
    const char *country = json_is_object(root) ? json_string_value(json_object_get(root, "country")) : NULL;
    country_code(g_country, country);
    g_partner_only = json_is_object(root) && json_is_true(json_object_get(root, "partner_only"));
    json_decref(root);
}

bool providers_fetch(void)
{
    static const char *const headers[] = { "Accept: application/json" };
    HttpResponse response;
    http_next_request(8, NULL, NULL);
    if (!http_request("GET", SERVICE_URLS, "Kasumi-3DS", headers, 1, NULL, 256 * 1024, &response)) {
        diagnostic_log("PROVIDER", "list failed: %s", response.error);
        return false;
    }
    json_error_t error;
    json_t *root = response.status == 200 ? json_loadb(response.body ? response.body : "", response.size, 0, &error) : NULL;
    http_response_free(&response);
    json_t *info = json_is_object(root) ? json_object_get(root, "gfnServiceInfo") : NULL;
    json_t *endpoints = json_is_object(info) ? json_object_get(info, "gfnServiceEndpoints") : NULL;
    GfnProvider fresh[PROVIDER_MAX];
    int priority[PROVIDER_MAX];
    unsigned count = 0;
    size_t index;
    json_t *entry;
    json_array_foreach(endpoints, index, entry) {
        if (count >= PROVIDER_MAX) break;
        if (!from_json(entry, &fresh[count])) continue;
        json_t *p = json_object_get(entry, "loginProviderPriority");
        priority[count] = json_is_integer(p) ? (int)json_integer_value(p) : 100;
        ++count;
    }
    /* NVIDIA's order (priority), NVIDIA itself first. */
    for (unsigned i = 1; i < count; ++i)
        for (unsigned j = i; j > 0 && priority[j] < priority[j - 1]; --j) {
            const GfnProvider tp = fresh[j]; fresh[j] = fresh[j - 1]; fresh[j - 1] = tp;
            const int tq = priority[j]; priority[j] = priority[j - 1]; priority[j - 1] = tq;
        }
    /* The recommendation: the first preferred login provider, else the
     * default one (OpenNOW Vita's order). Names or codes may be used. */
    char recommended[12] = "";
    json_t *preferred = json_is_object(info) ? json_object_get(info, "loginPreferredProviders") : NULL;
    const char *wanted = json_is_array(preferred) && json_array_size(preferred)
                         ? json_string_value(json_array_get(preferred, 0)) : NULL;
    if (!wanted && json_is_object(info)) wanted = json_string_value(json_object_get(info, "defaultProvider"));
    for (unsigned i = 0; wanted && i < count && !recommended[0]; ++i)
        if (!strcasecmp(fresh[i].code, wanted) || name_matches(fresh[i].name, wanted))
            snprintf(recommended, sizeof(recommended), "%.11s", fresh[i].code);
    const char *country = json_is_object(info) ? json_string_value(json_object_get(info, "clientCountryCode")) : NULL;
    /* NVIDIA names no provider for Australia, so players there signed in
     * with NVIDIA, got Japan's servers and were refused games (beta.29
     * report CKWQJZ). GeForce NOW there is Cloud.GG's. */
    /* Where NVIDIA names nobody, a partner may still be the only way in. */
    static const struct { const char *country, *provider; } partner_only[] = {
        { "AU", "PNT" },
    };
    bool only = false;
    for (size_t c = 0; !recommended[0] && country && c < sizeof(partner_only) / sizeof(partner_only[0]); ++c)
        if (!strcasecmp(country, partner_only[c].country))
            for (unsigned i = 0; i < count; ++i)
                if (!strcasecmp(fresh[i].code, partner_only[c].provider)) {
                    snprintf(recommended, sizeof(recommended), "%.11s", fresh[i].code);
                    only = true;
                }
    diagnostic_log("PROVIDER", "list %u providers, country=%s recommended=%s", count, country ? country : "?",
                   recommended[0] ? recommended : "-");
    json_decref(root);
    if (!count) return false;
    LightLock_Lock(&g_lock);
    memcpy(g_list, fresh, sizeof(GfnProvider) * count);
    g_count = count;
    snprintf(g_recommended, sizeof(g_recommended), "%s", recommended);
    country_code(g_country, country);
    g_partner_only = only;
    LightLock_Unlock(&g_lock);
    save();
    return true;
}

unsigned providers_count(void)
{
    LightLock_Lock(&g_lock);
    const unsigned count = g_count;
    LightLock_Unlock(&g_lock);
    return count;
}

bool providers_get(unsigned index, GfnProvider *out)
{
    LightLock_Lock(&g_lock);
    const bool ok = index < g_count;
    if (ok) *out = g_list[index];
    LightLock_Unlock(&g_lock);
    return ok;
}

bool providers_find(const char *code, GfnProvider *out)
{
    if (!code || !code[0]) return false;
    if (!strcasecmp(code, PROVIDER_NVIDIA)) {
        provider_nvidia(out);
        return true;
    }

    LightLock_Lock(&g_lock);
    bool found = false;
    for (unsigned i = 0; i < g_count && !found; ++i)
        if (!strcasecmp(g_list[i].code, code)) {
            *out = g_list[i];
            found = true;
        }
    LightLock_Unlock(&g_lock);
    return found;
}

void providers_recommended(GfnProvider *out)
{
    char code[12];
    LightLock_Lock(&g_lock);
    snprintf(code, sizeof(code), "%s", g_recommended);
    LightLock_Unlock(&g_lock);
    if (!providers_find(code, out)) provider_nvidia(out);
}

void providers_country(char *out, size_t size)
{
    LightLock_Lock(&g_lock);
    snprintf(out, size, "%s", g_country);
    LightLock_Unlock(&g_lock);
}

/* Countries named in messages: where partners sell GeForce NOW. */
const char *providers_country_name(const char *code)
{
    static const struct { const char *code, *name; } names[] = {
        { "AU", "Australia" }, { "JP", "Japan" }, { "KR", "Korea" }, { "TW", "Taiwan" },
        { "SG", "Singapore" }, { "MY", "Malaysia" }, { "ID", "Indonesia" }, { "TH", "Thailand" },
        { "PH", "the Philippines" }, { "VN", "Vietnam" }, { "CL", "Chile" }, { "AR", "Argentina" },
        { "PE", "Peru" }, { "CO", "Colombia" }, { "UY", "Uruguay" }, { "EC", "Ecuador" },
        { "ZA", "South Africa" }, { "AM", "Armenia" }, { "TR", "Turkey" }, { "SA", "Saudi Arabia" },
        { "AE", "the UAE" }, { "KW", "Kuwait" }, { "BH", "Bahrain" }, { "QA", "Qatar" }, { "OM", "Oman" },
        { "JO", "Jordan" }, { "IQ", "Iraq" }, { "IL", "Israel" }, { "IN", "India" },
    };
    for (size_t i = 0; code && i < sizeof(names) / sizeof(names[0]); ++i)
        if (!strcasecmp(code, names[i].code)) return names[i].name;
    /* A code we have no name for reads as itself ("EG"); anything that is
     * not two letters is not shown at all. */
    return code && strlen(code) == 2 && isalpha((unsigned char)code[0]) && isalpha((unsigned char)code[1])
        ? code : "your country";
}

bool providers_partner_here(GfnProvider *partner, bool *only)
{
    providers_recommended(partner);
    LightLock_Lock(&g_lock);
    if (only) *only = g_partner_only;
    LightLock_Unlock(&g_lock);
    return strcmp(partner->code, PROVIDER_NVIDIA) != 0;
}

void provider_set_active(const GfnProvider *provider)
{
    GfnProvider p;
    if (provider && valid(provider)) p = *provider;
    else provider_nvidia(&p);
    LightLock_Lock(&g_lock);
    g_active = p;
    g_active_set = true;
    LightLock_Unlock(&g_lock);
}

void provider_active(GfnProvider *out)
{
    LightLock_Lock(&g_lock);
    if (g_active_set) *out = g_active;
    else provider_nvidia(out);
    LightLock_Unlock(&g_lock);
}

void provider_base_url(char *out, size_t size)
{
    GfnProvider p;
    provider_active(&p);
    snprintf(out, size, "%s", p.url);
}

bool provider_is_nvidia(void)
{
    GfnProvider p;
    provider_active(&p);
    return !strcmp(p.code, PROVIDER_NVIDIA);
}
