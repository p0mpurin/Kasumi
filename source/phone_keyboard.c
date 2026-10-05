#include "phone_keyboard.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "diagnostic.h"
#include "gfn_input.h"
#include "qr.h"

#define PHONE_PORT 8080
/* Phones open spare connections ahead of time, so a few at once. */
#define MAX_CLIENTS 6
#define REQUEST_MAX 2048
#define QUEUE_SIZE 512
/* Keys sent per frame: quick enough for a pasted line, gentle on the
 * input channel. */
#define KEYS_PER_TICK 4

/* A connection reads its request, sends the answer a piece at a time
 * (the 3DS send buffer is small), then waits for the phone to hang up. */
enum { CLIENT_READING, CLIENT_SENDING, CLIENT_CLOSING };

typedef struct {
    int fd;
    int state;
    int length;
    u64 opened_at, state_at;
    char buffer[REQUEST_MAX + 1];
    /* The answer: a header made here and a body that is a constant. */
    char head[256];
    size_t head_length, head_sent;
    const char *body;
    size_t body_length, body_sent;
} Client;

typedef struct {
    uint16_t keycode, scancode, modifiers;
} QueuedKey;

static int g_listener = -1;
static Client g_clients[MAX_CLIENTS];
static char g_token[8];
static char g_url[64];
static unsigned char g_qr[QR_MAX_SIZE * QR_MAX_SIZE];
static int g_qr_size;
static QueuedKey g_queue[QUEUE_SIZE];
static unsigned g_head, g_tail;
static u64 g_last_request_at;
static unsigned g_keys_sent;
/* For the log (never what was typed): connections, pages, commands. */
static unsigned g_accepted, g_pages, g_commands, g_rejected;

/* The page the phone opens (kept small: it is sent in one go). */
static const char PAGE[] =
    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<title>Kasumi keyboard</title>\n"
    "<style>\n"
    "*{box-sizing:border-box}\n"
    "body{margin:0;padding:20px;background:#000;color:#eeeae2;font:16px system-ui,-apple-system,sans-serif}\n"
    "h1{margin:0 0 4px;font-size:20px;font-weight:600}\n"
    "p{margin:0 0 16px;color:#98958e;font-size:14px}\n"
    "textarea{width:100%;height:140px;padding:12px;background:#0c0c0f;color:#eeeae2;border:1px solid #3c3c44;border-radius:10px;font:18px system-ui,sans-serif;resize:none}\n"
    "textarea:focus{outline:none;border-color:#8db36a}\n"
    ".row{display:flex;gap:8px;margin-top:10px}\n"
    "button{flex:1;padding:14px 0;background:#17171c;color:#eeeae2;border:1px solid #3c3c44;border-radius:10px;font-size:15px}\n"
    "button:active{background:#24242a}\n"
    "#st{margin-top:14px;font-size:13px;color:#98958e}\n"
    "#st.bad{color:#e0483c}\n"
    "</style></head><body>\n"
    "<h1>Kasumi keyboard</h1>\n"
    "<p>Type here and it goes straight into the game on your 3DS.</p>\n"
    "<textarea id=\"t\" autocapitalize=\"off\" autocomplete=\"off\" autocorrect=\"off\" spellcheck=\"false\" placeholder=\"Start typing...\"></textarea>\n"
    "<div class=\"row\"><button data-k=\"e\">Enter</button><button data-k=\"b1\">Delete</button></div>\n"
    "<div class=\"row\"><button data-k=\"a\">Tab</button><button data-k=\"s\">Esc</button><button id=\"c\">Clear box</button></div>\n"
    "<div id=\"st\">Connected to your 3DS</div>\n"
    "<script>\n"
    "var t=document.getElementById('t'),st=document.getElementById('st'),old='',q=Promise.resolve();\n"
    "var url=location.pathname.replace(/\\/$/,'')+'/k';\n"
    "function say(ok,m){st.textContent=m;st.className=ok?'':'bad'}\n"
    "function send(b){q=q.then(function(){return fetch(url,{method:'POST',body:b}).then(function(r){r.ok?say(1,'Connected to your 3DS'):say(0,'The 3DS did not take that')},function(){say(0,\"Can't reach the 3DS. Is the game still running, on the same Wi-Fi?\")})})}\n"
    "function norm(s){return s.replace(/[\\u2018\\u2019]/g,\"'\").replace(/[\\u201c\\u201d]/g,'\"').replace(/\\u2026/g,'...').replace(/[\\u2013\\u2014]/g,'-').replace(/\\u00a0/g,' ')}\n"
    "function typed(s){return s.replace(/[^\\x09\\x0a\\x20-\\x7e]/g,'').length}\n"
    "t.addEventListener('input',function(){var v=norm(t.value),p=0;while(p<old.length&&p<v.length&&old[p]==v[p])p++;var gone=typed(old.slice(p));if(gone)send('b'+gone);if(typed(v.slice(p)))send('t'+v.slice(p));old=v});\n"
    "document.querySelectorAll('[data-k]').forEach(function(b){b.onclick=function(){send(b.dataset.k);t.focus()}});\n"
    "document.getElementById('c').onclick=function(){t.value='';old='';t.focus()};\n"
    "</script></body></html>\n";

bool phone_keyboard_running(void) { return g_listener >= 0; }
const char *phone_keyboard_url(void) { return g_url; }

int phone_keyboard_qr(const unsigned char **modules)
{
    *modules = g_qr;
    return g_listener >= 0 ? g_qr_size : 0;
}

bool phone_keyboard_connected(void)
{
    return g_listener >= 0 && g_last_request_at && osGetTime() - g_last_request_at < 120000;
}

static void set_nonblocking(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

bool phone_keyboard_start(void)
{
    if (g_listener >= 0) return true;
    const u32 ip = (u32)gethostid();
    if (!ip) return false;
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) return false;
    const int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(PHONE_PORT);
    /* The console's own address: the 3DS examples bind to it, and the
     * first build (bound to any address) was never reached. */
    address.sin_addr.s_addr = ip;
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(fd, MAX_CLIENTS) < 0) {
        diagnostic_log("PHONE", "listen failed errno=%d", errno);
        close(fd);
        return false;
    }
    set_nonblocking(fd);
    g_listener = fd;
    for (int i = 0; i < MAX_CLIENTS; ++i) g_clients[i].fd = -1;
    /* A random path, so only someone who saw the QR code can type. */
    static const char letters[] = "abcdefghjkmnpqrstuvwxyz23456789";
    u32 seed = (u32)svcGetSystemTick() ^ ((u32)osGetTime() * 2654435761u);
    for (int i = 0; i < 6; ++i) {
        seed = seed * 1103515245u + 12345u;
        g_token[i] = letters[(seed >> 16) % (sizeof(letters) - 1)];
    }
    g_token[6] = '\0';
    struct in_addr in;
    in.s_addr = ip;
    snprintf(g_url, sizeof(g_url), "http://%s:%d/%s", inet_ntoa(in), PHONE_PORT, g_token);
    g_qr_size = qr_encode(g_url, g_qr);
    g_head = g_tail = 0;
    g_last_request_at = 0;
    g_keys_sent = g_accepted = g_pages = g_commands = g_rejected = 0;
    diagnostic_log("PHONE", "keyboard page started on port %d (qr %d)", PHONE_PORT, g_qr_size);
    return true;
}

static void close_client(Client *c)
{
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

void phone_keyboard_stop(void)
{
    if (g_listener < 0) return;
    for (int i = 0; i < MAX_CLIENTS; ++i) close_client(&g_clients[i]);
    close(g_listener);
    g_listener = -1;
    g_head = g_tail = 0;
    diagnostic_log("PHONE", "keyboard page stopped: %u connections, %u pages, %u commands, %u keys, %u turned away",
                   g_accepted, g_pages, g_commands, g_keys_sent, g_rejected);
}

static void push_key(uint16_t keycode, uint16_t scancode, uint16_t modifiers)
{
    const unsigned next = (g_head + 1) % QUEUE_SIZE;
    if (next == g_tail) return; /* full: drop rather than stall */
    g_queue[g_head] = (QueuedKey){ keycode, scancode, modifiers };
    g_head = next;
}

/* One command from the page: t<text>, b<count> (backspaces), e (Enter),
 * a (Tab) or s (Esc). */
static void run_command(const char *body, int length)
{
    if (length < 1) return;
    switch (body[0]) {
    case 't':
        for (int i = 1; i < length; ++i) {
            const char c = body[i];
            uint16_t keycode, scancode, modifiers;
            if (c == '\n') push_key(0x0d, 0x1c, 0);
            else if (c == '\t') push_key(0x09, 0x0f, 0);
            else if (c >= 0x20 && c < 0x7f && gfn_input_key_for_char(c, &keycode, &scancode, &modifiers))
                push_key(keycode, scancode, modifiers);
        }
        break;
    case 'b': {
        char digits[8] = { 0 };
        memcpy(digits, body + 1, length - 1 < 7 ? (size_t)(length - 1) : 7);
        int count = atoi(digits);
        if (count < 1) count = 1;
        if (count > 200) count = 200;
        for (int i = 0; i < count; ++i) push_key(0x08, 0x0e, 0);
        break;
    }
    case 'e': push_key(0x0d, 0x1c, 0); break;
    case 'a': push_key(0x09, 0x0f, 0); break;
    case 's': push_key(0x1b, 0x01, 0); break;
    default: break;
    }
}

/* Queue the answer; pump_send() sends it over the next frames. The first
 * build sent it all at once and closed: whatever did not fit in the send
 * buffer was lost, and the phone showed a blank white page. */
static void respond(Client *c, const char *status, const char *type, const char *body)
{
    c->body = body;
    c->body_length = strlen(body);
    c->body_sent = c->head_sent = 0;
    const int n = snprintf(c->head, sizeof(c->head),
                           "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                           "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                           status, type, (unsigned)c->body_length);
    c->head_length = n > 0 && (size_t)n < sizeof(c->head) ? (size_t)n : 0;
    c->state = CLIENT_SENDING;
    c->state_at = osGetTime();
}

/* As much of the answer as the socket takes now; false on an error. */
static bool pump_send(Client *c)
{
    while (c->head_sent < c->head_length || c->body_sent < c->body_length) {
        const bool head = c->head_sent < c->head_length;
        const char *data = head ? c->head + c->head_sent : c->body + c->body_sent;
        const size_t left = head ? c->head_length - c->head_sent : c->body_length - c->body_sent;
        const ssize_t sent = send(c->fd, data, left > 1024 ? 1024 : left, 0);
        if (sent > 0) {
            if (head) c->head_sent += (size_t)sent;
            else c->body_sent += (size_t)sent;
            c->state_at = osGetTime();
        } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        } else {
            return false;
        }
    }
    /* All sent: say so, and close once the phone has read it. */
    shutdown(c->fd, SHUT_WR);
    c->state = CLIENT_CLOSING;
    c->state_at = osGetTime();
    return true;
}

/* Content-Length from the request headers (0 if none). */
static int content_length(const char *headers, int header_length)
{
    static const char key[] = "content-length:";
    for (int i = 0; i + (int)sizeof(key) - 1 < header_length; ++i) {
        if (i && headers[i - 1] != '\n') continue;
        int k = 0;
        while (key[k] && (headers[i + k] | 0x20) == key[k]) ++k;
        if (!key[k]) return atoi(headers + i + k);
    }
    return 0;
}

static void handle_request(Client *c, int header_length, int body_length)
{
    char method[8], path[96];
    if (sscanf(c->buffer, "%7s %95s", method, path) != 2) {
        respond(c, "400 Bad Request", "text/plain", "bad request");
        return;
    }
    char page[16], page_slash[16], command[16];
    snprintf(page, sizeof(page), "/%s", g_token);
    snprintf(page_slash, sizeof(page_slash), "/%s/", g_token);
    snprintf(command, sizeof(command), "/%s/k", g_token);
    if (!strcmp(method, "GET") && (!strcmp(path, page) || !strcmp(path, page_slash))) {
        g_last_request_at = osGetTime();
        if (!g_pages++) diagnostic_log("PHONE", "page opened");
        respond(c, "200 OK", "text/html; charset=utf-8", PAGE);
    } else if (!strcmp(method, "POST") && !strcmp(path, command)) {
        g_last_request_at = osGetTime();
        ++g_commands;
        run_command(c->buffer + header_length, body_length);
        respond(c, "200 OK", "text/plain", "ok");
    } else {
        diagnostic_log("PHONE", "unknown request %s (%u bytes)", method, (unsigned)strlen(path));
        respond(c, "404 Not Found", "text/plain", "not here");
    }
}

static void serve_client(Client *c, u64 now)
{
    if (c->state == CLIENT_SENDING) {
        if (!pump_send(c) || now - c->state_at > 15000) {
            diagnostic_log("PHONE", "answer cut off at %u of %u bytes", (unsigned)(c->head_sent + c->body_sent),
                           (unsigned)(c->head_length + c->body_length));
            close_client(c);
        }
        return;
    }
    if (c->state == CLIENT_CLOSING) {
        /* Read until the phone closes its side (or 3 s), so nothing it
         * still has to read is thrown away by an early close. */
        char scratch[256];
        const ssize_t got = recv(c->fd, scratch, sizeof(scratch), 0);
        if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK) || now - c->state_at > 3000)
            close_client(c);
        return;
    }
    if (c->length < REQUEST_MAX) {
        const ssize_t got = recv(c->fd, c->buffer + c->length, (size_t)(REQUEST_MAX - c->length), 0);
        if (got == 0) { close_client(c); return; }
        if (got > 0) {
            c->length += (int)got;
            c->buffer[c->length] = '\0';
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            close_client(c);
            return;
        }
    }
    const char *end = strstr(c->buffer, "\r\n\r\n");
    if (end) {
        const int header_length = (int)(end + 4 - c->buffer);
        const int body_length = content_length(c->buffer, header_length);
        if (body_length < 0 || header_length + body_length > REQUEST_MAX) {
            respond(c, "413 Payload Too Large", "text/plain", "too long");
            pump_send(c);
            return;
        }
        if (c->length >= header_length + body_length) {
            handle_request(c, header_length, body_length);
            if (!pump_send(c)) close_client(c);
            return;
        }
    }
    /* Spare connections a phone opened and never used, and stuck ones. */
    if (c->length >= REQUEST_MAX || now - c->opened_at > 10000) close_client(c);
}

void phone_keyboard_tick(WebRtcTransport *t)
{
    if (g_listener < 0) return;
    const u64 now = osGetTime();
    for (int k = 0; k < MAX_CLIENTS; ++k) {
        struct sockaddr_in from;
        socklen_t from_length = sizeof(from);
        const int fd = accept(g_listener, (struct sockaddr *)&from, &from_length);
        if (fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                static unsigned logged;
                if (logged++ < 3) diagnostic_log("PHONE", "accept errno=%d", errno);
            }
            break;
        }
        if (!g_accepted++) diagnostic_log("PHONE", "first connection");
        Client *slot = NULL;
        for (int i = 0; i < MAX_CLIENTS && !slot; ++i)
            if (g_clients[i].fd < 0) slot = &g_clients[i];
        /* All taken: a new connection is likelier to be the real request
         * than an old one that has said nothing, so that one makes room. */
        if (!slot) {
            Client *idle = NULL;
            for (int i = 0; i < MAX_CLIENTS; ++i)
                if (g_clients[i].state == CLIENT_READING && !g_clients[i].length &&
                    (!idle || g_clients[i].opened_at < idle->opened_at))
                    idle = &g_clients[i];
            if (idle) close_client(idle);
            slot = idle;
        }
        if (!slot) {
            ++g_rejected;
            close(fd);
            continue;
        }
        set_nonblocking(fd);
        slot->fd = fd;
        slot->state = CLIENT_READING;
        slot->length = 0;
        slot->buffer[0] = '\0';
        slot->opened_at = now;
    }
    for (int i = 0; i < MAX_CLIENTS; ++i)
        if (g_clients[i].fd >= 0) serve_client(&g_clients[i], now);
    for (int k = 0; k < KEYS_PER_TICK && g_head != g_tail; ++k) {
        const QueuedKey *key = &g_queue[g_tail];
        /* Not connected yet: keep the keys for when it is. */
        if (!webrtc_transport_send_key(t, key->keycode, key->scancode, key->modifiers)) break;
        g_tail = (g_tail + 1) % QUEUE_SIZE;
        ++g_keys_sent;
    }
}
