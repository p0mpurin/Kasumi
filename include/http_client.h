#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    long status;
    char *body;
    size_t size;
    char error[160];
} HttpResponse;

bool http_global_init(void);
void http_global_exit(void);
bool http_request(const char *method, const char *url, const char *user_agent,
                  const char *const *headers, size_t header_count,
                  const char *body, size_t max_response, HttpResponse *response);
void http_response_free(HttpResponse *response);
/* Abort the request in flight (from any thread); cleared by the next request. */
void http_cancel(void);
char *http_url_encode(const char *value);

