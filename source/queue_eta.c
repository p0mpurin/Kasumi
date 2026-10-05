#include "queue_eta.h"

#include <3ds.h>
#include <jansson.h>
#include <stdio.h>
#include <string.h>

#include "app_paths.h"
#include "diagnostic.h"
#include "file_worker.h"
#include "http_client.h"

#define QUEUE_ETA_PATH APP_DATA_DIR "/queue.json"
#define ETA_URL REPORT_BASE "/eta"
#define MAX_PROVIDERS 24

typedef struct {
    char code[12];
    float base, per_place;   /* the shared fit */
    int samples;
    float local_seconds;     /* this console's average wait (0: none yet) */
} Model;

static Model g_models[MAX_PROVIDERS];
static int g_count;
static LightLock g_lock = 1;

/* Under the lock. */
static Model *find(const char *code, bool create)
{
    for (int i = 0; i < g_count; ++i)
        if (!strcmp(g_models[i].code, code)) return &g_models[i];
    if (!create || g_count >= MAX_PROVIDERS) return NULL;
    Model *m = &g_models[g_count++];
    memset(m, 0, sizeof(*m));
    snprintf(m->code, sizeof(m->code), "%s", code);
    return m;
}

/* Under the lock. */
static void save(void)
{
    json_t *providers = json_object();
    json_t *local = json_object();
    for (int i = 0; i < g_count; ++i) {
        const Model *m = &g_models[i];
        if (m->samples)
            json_object_set_new(providers, m->code, json_pack("{s:f,s:f,s:i}", "base", (double)m->base,
                                                              "perPlace", (double)m->per_place, "n", m->samples));
        if (m->local_seconds > 0) json_object_set_new(local, m->code, json_real(m->local_seconds));
    }
    json_t *root = json_pack("{s:o,s:o}", "providers", providers, "local", local);
    /* In the background: a queue ending froze the loop on a slow card. */
    if (root) file_worker_save_json(QUEUE_ETA_PATH, root, JSON_COMPACT);
}

/* Under the lock: the "providers" object of a saved file or of /eta. */
static void read_providers(json_t *providers)
{
    const char *code;
    json_t *value;
    json_object_foreach(providers, code, value) {
        json_t *base = json_object_get(value, "base"), *per = json_object_get(value, "perPlace");
        json_t *n = json_object_get(value, "n");
        if (!json_is_number(base) || !json_is_number(per)) continue;
        Model *m = find(code, true);
        if (!m) break;
        m->base = (float)json_number_value(base);
        m->per_place = (float)json_number_value(per);
        m->samples = json_is_integer(n) ? (int)json_integer_value(n) : 1;
    }
}

void queue_eta_load(void)
{
    json_error_t error;
    json_t *root = json_load_file(QUEUE_ETA_PATH, 0, &error);
    LightLock_Lock(&g_lock);
    g_count = 0;
    if (json_is_object(root)) {
        read_providers(json_object_get(root, "providers"));
        const char *code;
        json_t *value;
        json_object_foreach(json_object_get(root, "local"), code, value) {
            Model *m = json_is_number(value) ? find(code, true) : NULL;
            if (m) m->local_seconds = (float)json_number_value(value);
        }
    }
    LightLock_Unlock(&g_lock);
    json_decref(root);
}

void queue_eta_fetch(void)
{
    static const char *const headers[] = { "Accept: application/json" };
    HttpResponse response;
    http_next_request(6, NULL, NULL);
    if (!http_request("GET", ETA_URL, "Kasumi-3DS", headers, 1, NULL, 32 * 1024, &response)) return;
    json_error_t error;
    json_t *root = response.status == 200 ? json_loadb(response.body ? response.body : "", response.size, 0, &error)
                                          : NULL;
    http_response_free(&response);
    json_t *providers = json_is_object(root) ? json_object_get(root, "providers") : NULL;
    if (json_is_object(providers)) {
        LightLock_Lock(&g_lock);
        read_providers(providers);
        save();
        const Model *nv = find("NVIDIA", false);
        if (nv && nv->samples)
            diagnostic_log("QUEUE", "shared estimate: NVIDIA %.0f s + %.3f s/place (%d queues)", (double)nv->base,
                           (double)nv->per_place, nv->samples);
        LightLock_Unlock(&g_lock);
    }
    json_decref(root);
}

bool queue_eta_expected(const char *provider, int place, float *seconds)
{
    bool ok = false;
    LightLock_Lock(&g_lock);
    const Model *m = find(provider && provider[0] ? provider : "NVIDIA", false);
    if (m && m->samples >= 5) {
        *seconds = m->base + m->per_place * (float)(place > 0 ? place : 1);
        ok = true;
    } else if (m && m->local_seconds > 0) {
        *seconds = m->local_seconds;
        ok = true;
    }
    LightLock_Unlock(&g_lock);
    return ok;
}

void queue_eta_learn(const char *provider, int place, float seconds)
{
    (void)place;
    if (seconds < 5.0f || seconds > 4.0f * 3600.0f) return;
    LightLock_Lock(&g_lock);
    Model *m = find(provider && provider[0] ? provider : "NVIDIA", true);
    if (m) {
        m->local_seconds = m->local_seconds > 0 ? m->local_seconds * 0.6f + seconds * 0.4f : seconds;
        save();
    }
    LightLock_Unlock(&g_lock);
}
