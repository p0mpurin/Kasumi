#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    long status;
    char *body;
    size_t size;
    char error[160];
    /* Where the request spent its time, for failure logs: curl's result,
     * then ms to DNS, TCP connect, TLS and the end (0 = not reached), and
     * the bytes of the request body that went out. */
    int curl_code;
    unsigned dns_ms, connect_ms, tls_ms, total_ms;
    unsigned long long uploaded;
} HttpResponse;

bool http_global_init(void);
void http_global_exit(void);
bool http_request(const char *method, const char *url, const char *user_agent,
                  const char *const *headers, size_t header_count,
                  const char *body, size_t max_response, HttpResponse *response);
void http_response_free(HttpResponse *response);
/* Box art, on its own thread and connection: never waits behind the
 * network worker's requests. http_art_cancel aborts the one in flight. */
bool http_request_art(const char *url, const char *user_agent, const char *const *headers, size_t header_count,
                      size_t max_response, HttpResponse *response);
void http_art_cancel(void);
/* Abort the request in flight (from any thread); cleared by the next request. */
void http_cancel(void);
/* For the next request only: a longer total timeout (large downloads) and a
 * progress callback (bytes received, bytes expected or 0). */
typedef void (*HttpProgress)(unsigned long long received, unsigned long long total, void *context);
void http_next_request(long timeout_seconds, HttpProgress progress, void *context);
char *http_url_encode(const char *value);
/* The console clock is clearly wrong (before this build, or years ahead),
 * so certificates cannot be checked and HTTPS fails. Writes the date the
 * 3DS thinks it is ("2011-01-01") when date is not NULL. */
bool http_clock_wrong(char *date, size_t size);

