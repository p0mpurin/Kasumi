#include "gfn_client.h"
#include "nvst_signal.h"
#include "nvst_protocol.h"
#include <jansson.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void diagnostic_log(const char *tag, const char *format, ...)
{
    (void)tag; (void)format;
}

/* Generated from the production functions by run_gfn_network_test.py. */
#include "gfn_network_under_test.inc"

static bool g_removed_retry_used;
static int last_ack;
static const char *i18n_text(const char *text) { return text; }
static void record(NvstSignal *s, const char *event) { (void)s; (void)event; }
static bool fail(NvstSignal *s, const char *text)
{
    (void)text; s->state = NVST_SIGNAL_ERROR; return false;
}
static bool send_json(NvstSignal *s, json_t *value)
{
    (void)s;
    last_ack = (int)json_integer_value(json_object_get(value, "ack"));
    return true;
}
static bool send_frame(NvstSignal *s, unsigned op, const void *data, size_t n)
{
    (void)s; (void)op; (void)data; (void)n; return true;
}
static bool heartbeat(NvstSignal *s) { (void)s; return true; }
#include "nvst_incoming_under_test.inc"

static void message(NvstSignal *s, const char *json)
{
    assert(incoming(s, 1, (const uint8_t *)json, strlen(json)));
}

static void signaling_v3(void)
{
    char url[256];
    assert(nvst_sign_in_url(url, sizeof(url), "wss://rig.example/nvst", "peer-test", "session-test", false));
    assert(!strcmp(url, "wss://rig.example/nvst/sign_in?peer_id=peer-test&version=3.0&peer_role=1&pairing_id=session-test"));
    assert(nvst_sign_in_url(url, sizeof(url), "wss://rig.example/nvst", "peer-test", "session-test", true));
    assert(strstr(url, "&reconnect=1"));
    assert(!nvst_sign_in_url(url, 8, "wss://rig.example", "peer-test", "session-test", false));

    NvstSignal *s = calloc(1, sizeof(*s));
    assert(s);
    s->state = NVST_SIGNAL_WAITING;
    strcpy(s->peer_name, "peer-test");
    /* Sanitized shapes captured from the official browser, October 27. */
    message(s, "{\"peer_info\":{\"browser\":\"Firefox\",\"browserVersion\":\"158.0\","
               "\"connected\":true,\"id\":2,\"name\":\"peer-test\",\"peer_role\":1,"
               "\"resolution\":\"\",\"signalingVersion\":\"3.0\",\"version\":3}}");
    assert(s->peer_assigned && s->local_peer_id == 2);
    message(s, "{\"ackid\":1,\"peer_info\":{\"connected\":true,\"id\":1,"
               "\"name\":\"OneSdkServer-test\",\"peer_role\":0,\"signalingVersion\":\"3.0\",\"version\":3}}");
    assert(s->remote_peer_id == 1 && last_ack == 1);
    message(s, "{\"ackid\":2,\"peer_msg\":{\"from\":1,\"to\":2,"
               "\"msg\":\"{\\\"type\\\":\\\"offer\\\",\\\"sdp\\\":\\\"v=0\\\\r\\\\n\\\",\\\"nvstSdp\\\":\\\"test\\\"}\"}}");
    assert(s->state == NVST_SIGNAL_OFFER && s->offer_size > 0 && last_ack == 2);
    assert(s->offer_nvst_size == 4);
    free(s->offer_sdp); free(s->offer_nvst_sdp); free(s);
    puts("NVST v3 URL and browser message sequence tests passed");
}

static void apply(GfnClient *client, const char *text)
{
    json_error_t error;
    json_t *session = json_loads(text, JSON_REJECT_DUPLICATES, &error);
    assert(session);
    parse_session_network(client, session);
    json_decref(session);
}

int main(void)
{
    signaling_v3();
    /* GfnClient contains the catalog: keep it off the stack. */
    GfnClient *client = calloc(1, sizeof(*client));
    assert(client);
    apply(client, "{\"connectionInfo\":[{\"usage\":14,\"ip\":\"queue.example\","
                  "\"resourcePath\":\"/nvst/\",\"port\":443}]}");
    assert(!strcmp(client->signaling_url, "wss://queue.example:443/nvst/"));

    /* Queued seat moves to a different rig before the initial sign-in. */
    apply(client, "{\"connectionInfo\":[{\"usage\":14,\"ip\":\"ready.example\","
                  "\"resourcePath\":\"wss://ready.example/nvst/\",\"port\":443},"
                  "{\"usage\":2,\"ip\":\"media.example\",\"port\":49000}]}");
    assert(!strcmp(client->signaling_url, "wss://ready.example/nvst/"));
    assert(!strcmp(client->media_ip, "media.example"));
    assert(client->media_port == 49000);

    /* A new higher-priority media connection must replace old usage 14. */
    apply(client, "{\"connectionInfo\":[{\"usage\":17,\"ip\":\"other.example\",\"port\":49001}]}");
    assert(!strcmp(client->media_ip, "other.example"));
    assert(client->media_port == 49001);
    assert(!strcmp(client->signaling_url, "wss://ready.example/nvst/"));

    /* Explicit URL wins over a connection-derived URL in this response. */
    apply(client, "{\"signalingUrl\":\"wss://explicit.example/nvst/\",\"connectionInfo\":["
                  "{\"usage\":14,\"ip\":\"fallback.example\",\"resourcePath\":\"/nvst/\"}]}");
    assert(!strcmp(client->signaling_url, "wss://explicit.example/nvst/"));

    /* Partial polling responses retain the previous usable endpoints. */
    apply(client, "{\"connectionInfo\":null}");
    apply(client, "{\"signalingUrl\":\"\",\"connectionInfo\":[]}");
    assert(!strcmp(client->signaling_url, "wss://explicit.example/nvst/"));
    assert(!strcmp(client->media_ip, "other.example"));
    assert(client->media_port == 49001);

    /* String usage, array IP, HTTPS conversion and relative path migration. */
    apply(client, "{\"connectionInfo\":[{\"usage\":\"14\",\"ip\":[\"array.example\"],"
                  "\"resourcePath\":\"https://array.example/nvst/\"}]}");
    assert(!strcmp(client->signaling_url, "wss://array.example/nvst/"));
    apply(client, "{\"connectionInfo\":[{\"usage\":14,\"ip\":\"relative.example\","
                  "\"resourcePath\":\"/new-nvst/\"}]}");
    assert(!strcmp(client->signaling_url, "wss://relative.example:443/new-nvst/"));
    free(client);
    puts("GFN network migration tests passed");
    return 0;
}
