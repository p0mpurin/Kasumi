#include "ws_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { unsigned messages, pings, closes; size_t size; char text[64]; } Capture;
static bool capture(void *context, unsigned op, const uint8_t *data, size_t n)
{
    Capture *c = context;
    if (op == 1) {
        ++c->messages; c->size = n;
        if (n < sizeof(c->text)) { memcpy(c->text, data, n); c->text[n] = 0; }
    }
    if (op == 9) ++c->pings;
    if (op == 8) ++c->closes;
    return true;
}
static void fragmented(void)
{
    /* Text fragments with an interleaved ping; split at every byte boundary. */
    const uint8_t bytes[] = {1,3,'a','b','c',137,1,'?',128,3,'d','e','f',129,2,'o','k'};
    for (size_t split = 0; split <= sizeof(bytes); ++split) {
        WsParser p = {0}; Capture c = {0};
        assert(ws_parser_feed(&p, bytes, split, capture, &c));
        assert(ws_parser_feed(&p, bytes+split, sizeof(bytes)-split, capture, &c));
        assert(c.messages == 2 && c.pings == 1 && !strcmp(c.text,"ok"));
        assert(p.wire_size == 0 && p.fragmented_opcode == 0);
        ws_parser_free(&p);
    }
    WsParser p = {0}; Capture c = {0};
    for (size_t i=0; i<sizeof(bytes); ++i) assert(ws_parser_feed(&p,bytes+i,1,capture,&c));
    assert(c.messages == 2 && c.pings == 1); ws_parser_free(&p);
}
static void encoding(void)
{
    /* RFC 6455 section 5.7 masking example, independent of our encoder. */
    const uint8_t mask[] = {0x37,0xfa,0x21,0x3d};
    const uint8_t expected[] = {0x81,0x85,0x37,0xfa,0x21,0x3d,0x7f,0x9f,0x4d,0x51,0x58};
    uint8_t out[32];
    assert(ws_encode_frame(out,sizeof(out),1,(const uint8_t *)"Hello",5,mask) == sizeof(expected));
    assert(!memcmp(out,expected,sizeof(expected)));
    assert(ws_encode_frame(out,10,1,(const uint8_t *)"Hello",5,mask) == 0);
    const size_t lengths[] = {0,125,126,65535,65536,WS_MESSAGE_LIMIT};
    for (size_t j=0; j<sizeof(lengths)/sizeof(lengths[0]); ++j) {
        size_t n = lengths[j], h = n<126 ? 2 : n<65536 ? 4 : 10;
        uint8_t *plain = calloc(n+1,1), *wire = malloc(n+14);
        assert(plain && wire);
        assert(ws_encode_frame(wire,n+14,1,plain,n,mask) == n+h+4);
        for (size_t i=0; i<n; ++i) assert(wire[h+4+i] == mask[i&3]);
        /* Construct an unmasked server frame using the header and plain data. */
        wire[1] &= 127; memcpy(wire+h,plain,n);
        WsParser p = {0}; Capture c = {0};
        for (size_t pos=0; pos<n+h;) {
            size_t chunk = n+h-pos > 997 ? 997 : n+h-pos;
            assert(ws_parser_feed(&p,wire+pos,chunk,capture,&c)); pos += chunk;
        }
        assert(c.messages == 1 && c.size == n);
        assert(p.message_capacity <= WS_MESSAGE_LIMIT+1);
        ws_parser_free(&p); free(plain); free(wire);
    }
}
static void bad_frames(void)
{
    const uint8_t frames[][10] = {
        {0x81,0x80}, /* server must not mask */
        {0xc1,0}, /* unnegotiated extension */
        {0x80,0}, /* orphan continuation */
        {0x09,0}, /* fragmented ping */
        {0x89,126}, /* oversized ping */
        {0x83,0}, /* reserved opcode */
        {0x88,1,0}, /* incomplete close code */
        {0x81,126,0,1,'a'}, /* noncanonical length */
        {0x81,127,0,0,0,0,0,2,0,1} /* 128 KiB + 1 */
    };
    const size_t sizes[] = {2,2,2,2,2,2,3,5,10};
    for (size_t i=0; i<sizeof(sizes)/sizeof(sizes[0]); ++i) {
        WsParser p = {0}; Capture c = {0};
        assert(!ws_parser_feed(&p,frames[i],sizes[i],capture,&c));
        assert(p.error); ws_parser_free(&p);
    }
    uint8_t restart[] = {1,1,'a',129,1,'b'};
    WsParser p = {0}; Capture c = {0};
    assert(!ws_parser_feed(&p,restart,sizeof(restart),capture,&c)); ws_parser_free(&p);
    uint8_t close[] = {136,2,3,232};
    assert(ws_parser_feed(&p,close,sizeof(close),capture,&c));
    assert(p.closed && c.closes == 1); ws_parser_free(&p);
}
static void coalesced_limit(void)
{
    size_t n = WS_MESSAGE_LIMIT;
    uint8_t *bytes = calloc(n + 32, 1);
    assert(bytes);
    bytes[0] = 129; bytes[1] = 127; bytes[7] = 2;
    bytes[n+10] = 129; bytes[n+11] = 2;
    bytes[n+12] = 'o'; bytes[n+13] = 'k';
    WsParser p = {0}; Capture c = {0};
    assert(ws_parser_feed(&p, bytes, n-5, capture, &c));
    assert(ws_parser_feed(&p, bytes+n-5, 19, capture, &c));
    assert(c.messages == 2 && !strcmp(c.text, "ok"));
    ws_parser_free(&p); free(bytes);
}

static void handshake(void)
{
    const char *accept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
    const char *good = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: WebSocket\r\n"
        "Connection: keep-alive, Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
        "Sec-WebSocket-Protocol: x-nv-sessionid.test\r\n\r\n";
    assert(ws_validate_upgrade(good,strlen(good),accept,"x-nv-sessionid.test"));
    assert(!ws_validate_upgrade(good,strlen(good),"wrong","x-nv-sessionid.test"));
    assert(!ws_validate_upgrade(good,strlen(good),accept,"different"));
    for (size_t n=0; n<strlen(good); ++n) assert(!ws_validate_upgrade(good,n,accept,"x-nv-sessionid.test"));
    const char *bad = "HTTP/1.1 101 Switching Protocols\r\n\r\n";
    assert(!ws_validate_upgrade(bad,strlen(bad),accept,""));
    /* Frame received in the same read as the HTTP response must be consumed. */
    const uint8_t first_frame[] = {129,2,'{','}'};
    WsParser p = {0}; Capture c = {0};
    assert(ws_parser_feed(&p,first_frame,sizeof(first_frame),capture,&c));
    assert(c.messages == 1 && !strcmp(c.text,"{}")); ws_parser_free(&p);
}
int main(void)
{
    handshake(); encoding(); fragmented(); bad_frames(); coalesced_limit();
    puts("PASS: upgrade validation, RFC masking, split/coalesced frames, fragments/ping, bounds, close");
    return 0;
}
