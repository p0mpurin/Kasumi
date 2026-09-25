#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WS_MESSAGE_LIMIT (128u * 1024u)
typedef bool (*WsMessageFn)(void *context, unsigned opcode, const uint8_t *data, size_t size);
typedef struct {
    uint8_t *wire, *message;
    size_t wire_size, wire_capacity, message_size, message_capacity;
    unsigned fragmented_opcode;
    unsigned frames;
    bool closed;
    const char *error;
} WsParser;

void ws_parser_free(WsParser *parser);
bool ws_parser_feed(WsParser *parser, const uint8_t *data, size_t size,
                    WsMessageFn callback, void *context);
size_t ws_encode_frame(uint8_t *output, size_t capacity, unsigned opcode,
                       const uint8_t *data, size_t size, const uint8_t mask[4]);
/* Takes only the HTTP header, never frame bytes. */
bool ws_validate_upgrade(const char *header, size_t size, const char *accept,
                         const char *protocol);
