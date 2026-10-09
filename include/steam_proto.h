#pragma once

/* Just enough protobuf for Steam Remote Play: the messages are small and
 * few, so they are written and read field by field (field numbers from
 * SteamDatabase/Protobufs: steammessages_remoteclient_discovery.proto,
 * steammessages_remoteplay.proto, steammessages_hiddevices.proto). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { PB_VARINT = 0, PB_FIXED64 = 1, PB_BYTES = 2, PB_FIXED32 = 5 };

typedef struct {
    uint8_t *data;
    size_t cap, len;
    bool overflow;
} PbWriter;

void pb_writer(PbWriter *w, uint8_t *data, size_t cap);
void pb_varint(PbWriter *w, unsigned field, uint64_t value);
/* int32 and enums: negative values take ten bytes, as protobuf does. */
void pb_int(PbWriter *w, unsigned field, int64_t value);
void pb_bool(PbWriter *w, unsigned field, bool value);
void pb_fixed32(PbWriter *w, unsigned field, uint32_t value);
void pb_fixed64(PbWriter *w, unsigned field, uint64_t value);
void pb_float(PbWriter *w, unsigned field, float value);
void pb_bytes(PbWriter *w, unsigned field, const void *data, size_t size);
void pb_string(PbWriter *w, unsigned field, const char *text);
/* A nested message already written into its own writer. */
void pb_message(PbWriter *w, unsigned field, const PbWriter *inner);
bool pb_ok(const PbWriter *w);

typedef struct {
    const uint8_t *p, *end;
} PbReader;

typedef struct {
    unsigned field, type;
    uint64_t value;          /* varint and fixed fields */
    const uint8_t *data;     /* length-delimited fields */
    size_t size;
} PbField;

void pb_reader(PbReader *r, const void *data, size_t size);
/* The next field; false at the end or on malformed input. */
bool pb_next(PbReader *r, PbField *f);
/* Copy a string field (always terminated). */
void pb_copy_string(const PbField *f, char *out, size_t size);
