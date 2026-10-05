#include "net_memory.h"

#include <3ds.h>
#include <jansson.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_paths.h"
#include "diagnostic.h"
#include "file_worker.h"

#define NET_MEMORY_PATH APP_DATA_DIR "/network-memory.json"
#define NET_MEMORY_DAYS 14
#define NET_MEMORY_MAX 8

/* Same test as the Weak-mode tip after a session. */
static bool choppy(unsigned seconds, unsigned lost, unsigned repeated)
{
    const unsigned minutes = seconds / 60;
    return minutes && (lost / minutes >= 3 || repeated / minutes >= 15);
}

static bool current_ssid(char out[40])
{
    char ssid[64];
    memset(ssid, 0, sizeof(ssid));
    if (R_FAILED(ACU_GetSSID(ssid))) return false;
    ssid[32] = '\0';
    snprintf(out, 40, "%s", ssid);
    return out[0] != '\0';
}

bool net_memory_choppy_here(void)
{
    char ssid[40];
    if (!current_ssid(ssid)) return false;
    json_error_t error;
    json_t *root = json_load_file(NET_MEMORY_PATH, 0, &error);
    json_t *entry = json_is_object(root) ? json_object_get(root, ssid) : NULL;
    json_t *at = json_is_object(entry) ? json_object_get(entry, "at") : NULL;
    const bool recent = json_is_integer(at) &&
                        (int64_t)time(NULL) - json_integer_value(at) < NET_MEMORY_DAYS * 86400;
    const bool result = recent && json_is_true(json_object_get(entry, "choppy"));
    json_decref(root);
    return result;
}

void net_memory_note(bool weak, unsigned seconds, unsigned lost, unsigned repeated)
{
    /* Two minutes say something; Weak sessions say nothing about Standard. */
    if (seconds < 120 || weak) return;
    char ssid[40];
    if (!current_ssid(ssid)) return;
    file_worker_flush();
    json_error_t error;
    json_t *root = json_load_file(NET_MEMORY_PATH, 0, &error);
    if (!json_is_object(root)) {
        json_decref(root);
        root = json_object();
    }
    const bool bad = choppy(seconds, lost, repeated);
    json_object_set_new(root, ssid, json_pack("{s:b,s:I}", "choppy", (int)bad, "at", (json_int_t)time(NULL)));
    /* Forget the oldest networks beyond a handful. */
    while (json_object_size(root) > NET_MEMORY_MAX) {
        const char *oldest = NULL;
        char oldest_key[40] = "";
        json_int_t oldest_at = 0;
        const char *key;
        json_t *value;
        json_object_foreach(root, key, value) {
            json_t *at = json_object_get(value, "at");
            const json_int_t t = json_is_integer(at) ? json_integer_value(at) : 0;
            if (!oldest || t < oldest_at) { oldest = key; oldest_at = t; }
        }
        if (!oldest) break;
        snprintf(oldest_key, sizeof(oldest_key), "%s", oldest);
        json_object_del(root, oldest_key);
    }
    file_worker_save_json(NET_MEMORY_PATH, root, JSON_COMPACT);
    diagnostic_log("NET", "network memory: last Standard session here %s", bad ? "choppy" : "smooth");
}
