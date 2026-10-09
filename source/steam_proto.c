#include "steam_proto.h"

#include <string.h>

void pb_writer(PbWriter *w, uint8_t *data, size_t cap)
{
    w->data = data;
    w->cap = cap;
    w->len = 0;
    w->overflow = false;
}

static void put(PbWriter *w, const void *data, size_t size)
{
    if (w->overflow || size > w->cap - w->len) {
        w->overflow = true;
        return;
    }
    memcpy(w->data + w->len, data, size);
    w->len += size;
}

static void raw_varint(PbWriter *w, uint64_t value)
{
    uint8_t out[10];
    size_t n = 0;
    do {
        out[n] = (uint8_t)(value & 0x7f);
        value >>= 7;
        if (value) out[n] |= 0x80;
        ++n;
    } while (value && n < sizeof(out));
    put(w, out, n);
}

static void key(PbWriter *w, unsigned field, unsigned type)
{
    raw_varint(w, ((uint64_t)field << 3) | type);
}

void pb_varint(PbWriter *w, unsigned field, uint64_t value)
{
    key(w, field, PB_VARINT);
    raw_varint(w, value);
}

void pb_int(PbWriter *w, unsigned field, int64_t value) { pb_varint(w, field, (uint64_t)value); }

void pb_bool(PbWriter *w, unsigned field, bool value) { pb_varint(w, field, value ? 1 : 0); }

void pb_fixed32(PbWriter *w, unsigned field, uint32_t value)
{
    const uint8_t out[4] = { (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24) };
    key(w, field, PB_FIXED32);
    put(w, out, sizeof(out));
}

void pb_fixed64(PbWriter *w, unsigned field, uint64_t value)
{
    uint8_t out[8];
    for (unsigned i = 0; i < 8; ++i) out[i] = (uint8_t)(value >> (8 * i));
    key(w, field, PB_FIXED64);
    put(w, out, sizeof(out));
}

void pb_float(PbWriter *w, unsigned field, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    pb_fixed32(w, field, bits);
}

void pb_bytes(PbWriter *w, unsigned field, const void *data, size_t size)
{
    key(w, field, PB_BYTES);
    raw_varint(w, size);
    put(w, data, size);
}

void pb_string(PbWriter *w, unsigned field, const char *text)
{
    pb_bytes(w, field, text, strlen(text));
}

void pb_message(PbWriter *w, unsigned field, const PbWriter *inner)
{
    if (inner->overflow) w->overflow = true;
    pb_bytes(w, field, inner->data, inner->len);
}

bool pb_ok(const PbWriter *w) { return !w->overflow; }

void pb_reader(PbReader *r, const void *data, size_t size)
{
    r->p = data;
    r->end = r->p + size;
}

static bool read_varint(PbReader *r, uint64_t *out)
{
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (r->p >= r->end) return false;
        const uint8_t byte = *r->p++;
        value |= (uint64_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) {
            *out = value;
            return true;
        }
    }
    return false;
}

bool pb_next(PbReader *r, PbField *f)
{
    uint64_t k;
    if (r->p >= r->end || !read_varint(r, &k)) return false;
    f->field = (unsigned)(k >> 3);
    f->type = (unsigned)(k & 7);
    f->value = 0;
    f->data = NULL;
    f->size = 0;
    switch (f->type) {
    case PB_VARINT:
        return read_varint(r, &f->value);
    case PB_FIXED64:
        if (r->end - r->p < 8) return false;
        for (unsigned i = 0; i < 8; ++i) f->value |= (uint64_t)r->p[i] << (8 * i);
        r->p += 8;
        return true;
    case PB_FIXED32:
        if (r->end - r->p < 4) return false;
        for (unsigned i = 0; i < 4; ++i) f->value |= (uint64_t)r->p[i] << (8 * i);
        r->p += 4;
        return true;
    case PB_BYTES: {
        uint64_t size;
        if (!read_varint(r, &size) || size > (uint64_t)(r->end - r->p)) return false;
        f->data = r->p;
        f->size = (size_t)size;
        r->p += size;
        return true;
    }
    default:
        return false;
    }
}

void pb_copy_string(const PbField *f, char *out, size_t size)
{
    if (!size) return;
    size_t n = f->data ? f->size : 0;
    if (n >= size) n = size - 1;
    if (n) memcpy(out, f->data, n);
    out[n] = '\0';
}
