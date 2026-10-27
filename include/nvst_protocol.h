#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* Observed in the official browser client: v3 registers the peer through
 * sign_in and receives both peer announcements before the server's offer.
 * reconnect=1 belongs only to a sign-in retaining the previous peer name. */
static inline bool nvst_sign_in_url(char *out, size_t capacity, const char *base,
                                    const char *peer, const char *session, bool reuse)
{
    int n = snprintf(out, capacity,
        "%s/sign_in?peer_id=%s&version=3.0&peer_role=1&pairing_id=%s%s",
        base, peer, session, reuse ? "&reconnect=1" : "");
    return n >= 0 && (size_t)n < capacity;
}
