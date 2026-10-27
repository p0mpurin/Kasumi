#include "platform.h"
#include "diagnostic.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <psp2/net/net.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/* getaddrinfo for curl and libpeer, in place of newlib's. Newlib's asks the
 * system resolver once, with no retry, through gethostbyname's static
 * buffers, while the network worker, the art thread and the stream's
 * signalling resolve at the same time: sign-ins failed with "Could not
 * resolve hostname". Here each lookup has its own resolver, a few tries,
 * and recent answers are kept (and a stale one beats none). IPv4 only, as
 * the Vita is. */

#define RESOLVE_TIMEOUT_US (2 * 1000 * 1000)
#define RESOLVE_RETRIES 3
#define CACHE_SIZE 16
#define CACHE_FRESH_MS (5 * 60 * 1000)

typedef struct {
    char host[128];
    struct in_addr address;
    u64 at;
} CacheEntry;

static CacheEntry g_cache[CACHE_SIZE];
/* Created on first use (platform_vita.h). */
static PlatLock g_cache_lock = PLAT_LOCK_INIT;

static void cache_lock(void) { plat_lock(&g_cache_lock); }

/* fresh_only: an answer young enough to use without asking again. */
static bool cache_find(const char *host, bool fresh_only, struct in_addr *out)
{
    bool found = false;
    cache_lock();
    for (unsigned i = 0; i < CACHE_SIZE; ++i) {
        const CacheEntry *e = &g_cache[i];
        if (!e->at || strcmp(e->host, host)) continue;
        if (fresh_only && plat_time_ms() - e->at > CACHE_FRESH_MS) break;
        *out = e->address;
        found = true;
        break;
    }
    plat_unlock(&g_cache_lock);
    return found;
}

static void cache_store(const char *host, struct in_addr address)
{
    if (strlen(host) >= sizeof(g_cache[0].host)) return;
    cache_lock();
    unsigned slot = 0;
    for (unsigned i = 0; i < CACHE_SIZE; ++i) {
        if (g_cache[i].at && !strcmp(g_cache[i].host, host)) {
            slot = i;
            break;
        }
        if (g_cache[i].at < g_cache[slot].at) slot = i;
    }
    strcpy(g_cache[slot].host, host);
    g_cache[slot].address = address;
    g_cache[slot].at = plat_time_ms();
    plat_unlock(&g_cache_lock);
}

static int resolve(const char *host, struct in_addr *out)
{
    if (cache_find(host, true, out)) return 0;
    const u64 started = plat_time_ms();
    int result = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const int rid = sceNetResolverCreate("kasumi", NULL, 0);
        if (rid < 0) {
            result = rid;
            continue;
        }
        SceNetInAddr address;
        memset(&address, 0, sizeof(address));
        result = sceNetResolverStartNtoa(rid, host, &address, RESOLVE_TIMEOUT_US, RESOLVE_RETRIES, 0);
        sceNetResolverDestroy(rid);
        if (result >= 0 && address.s_addr) {
            out->s_addr = address.s_addr;
            cache_store(host, *out);
            /* Once a run, so a log shows the lookups work. */
            static bool logged;
            if (!logged) {
                logged = true;
                diagnostic_log("dns", "%s: found in %u ms", host, (unsigned)(plat_time_ms() - started));
            }
            return 0;
        }
    }
    const bool stale = cache_find(host, false, out);
    diagnostic_log("dns", "%s: lookup failed 0x%08X after %u ms%s", host, (unsigned)result,
                   (unsigned)(plat_time_ms() - started), stale ? ", using the last answer" : "");
    return stale ? 0 : -1;
}

static int parse_port(const char *service, int flags, unsigned short *port)
{
    *port = 0;
    if (!service || !service[0]) return 0;
    char *end = NULL;
    const long value = strtol(service, &end, 10);
    if (end && *end == '\0' && value >= 0 && value <= 65535) {
        *port = (unsigned short)value;
        return 0;
    }
    if (flags & AI_NUMERICSERV) return EAI_NONAME;
    if (!strcmp(service, "https")) *port = 443;
    else if (!strcmp(service, "http")) *port = 80;
    else return EAI_SERVICE;
    return 0;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
    if (!res) return EAI_FAIL;
    *res = NULL;
    const int flags = hints ? hints->ai_flags : 0;
    const int family = hints ? hints->ai_family : AF_UNSPEC;
    if (family != AF_UNSPEC && family != AF_INET) return EAI_FAMILY;
    if (!node && !service) return EAI_NONAME;
    unsigned short port;
    const int service_error = parse_port(service, flags, &port);
    if (service_error) return service_error;

    struct in_addr address;
    if (!node) address.s_addr = htonl((flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK);
    else if (inet_pton(AF_INET, node, &address) != 1) {
        if (flags & AI_NUMERICHOST) return EAI_NONAME;
        if (resolve(node, &address) != 0) return EAI_NONAME;
    }

    /* One block: the entry, then its address (freeaddrinfo frees both). */
    struct addrinfo *ai = calloc(1, sizeof(*ai) + sizeof(struct sockaddr_in));
    if (!ai) return EAI_MEMORY;
    struct sockaddr_in *sin = (struct sockaddr_in *)(ai + 1);
    sin->sin_len = sizeof(*sin);
    sin->sin_family = AF_INET;
    sin->sin_port = htons(port);
    sin->sin_addr = address;
    ai->ai_flags = flags;
    ai->ai_family = AF_INET;
    ai->ai_socktype = hints ? hints->ai_socktype : 0;
    ai->ai_protocol = hints ? hints->ai_protocol : 0;
    ai->ai_addrlen = sizeof(*sin);
    ai->ai_addr = (struct sockaddr *)sin;
    if ((flags & AI_CANONNAME) && node) ai->ai_canonname = strdup(node);
    *res = ai;
    return 0;
}

/* Newlib keeps this with its getaddrinfo, and OpenSSL calls it: defined
 * here so that one is never linked. */
const char *gai_strerror(int error)
{
    switch (error) {
    case 0: return "Success";
    case EAI_NONAME: return "Could not resolve the host name";
    case EAI_SERVICE: return "Service not supported";
    case EAI_FAMILY: return "Address family not supported";
    case EAI_MEMORY: return "Out of memory";
    default: return "Name lookup failed";
    }
}

void freeaddrinfo(struct addrinfo *ai)
{
    while (ai) {
        struct addrinfo *next = ai->ai_next;
        free(ai->ai_canonname);
        free(ai);
        ai = next;
    }
}
