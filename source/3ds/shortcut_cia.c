#include "shortcut_cia.h"

#include <mbedtls/sha256.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* resources/shortcut.cia, linked in by the Makefile. */
extern const uint8_t _binary_shortcut_cia_start[];
extern const uint8_t _binary_shortcut_cia_end[];

#define MEDIA_UNIT 0x200
#define EXEFS_FILES 10
/* bannertool's banner model: the CGFX unpacks to this size, and its one
 * texture (256x128 RGBA4444) sits at this offset. Found by building banners
 * from plain red, plain blue and a gradient and comparing them. */
#define CGFX_SIZE 71040u
#define CGFX_TEXTURE_OFFSET 0x1580u
#define CGFX_TEXTURE_SIZE (SHORTCUT_BANNER_WIDTH * SHORTCUT_BANNER_HEIGHT * 2u)

static size_t align_up(size_t value, size_t to) { return (value + to - 1) / to * to; }

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

static void put_le32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static void put_le64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static void put_be64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (56 - 8 * i)); }

static void sha256(const uint8_t *data, size_t size, uint8_t out[32])
{
    mbedtls_sha256_ret(data, size, out, 0);
}

static bool set_error(char *error, size_t size, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (error && size) vsnprintf(error, size, format, args);
    va_end(args);
    return false;
}

/* Ticket and TMD start with a signature: its type, the signature, padding. */
static size_t signature_size(uint32_t type)
{
    switch (type) {
    case 0x10003: return 4 + 0x200 + 0x3C;
    case 0x10004: return 4 + 0x100 + 0x3C;
    case 0x10005: return 4 + 0x3C + 0x40;
    }
    return 0;
}

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t ticket, tmd, content, meta;          /* offsets in the CIA */
    size_t ticket_size, tmd_size, meta_size;
    size_t content_size;
    size_t exefs;                                /* offset in the NCCH */
    struct { char name[8]; const uint8_t *data; uint32_t size; } files[EXEFS_FILES];
} Template;

static bool parse_template(Template *t, char *error, size_t error_size)
{
    memset(t, 0, sizeof(*t));
    t->data = _binary_shortcut_cia_start;
    t->size = (size_t)(_binary_shortcut_cia_end - _binary_shortcut_cia_start);
    const uint8_t *c = t->data;
    if (t->size < 0x2040 || le32(c) != 0x2020) return set_error(error, error_size, "shortcut template missing");
    const size_t cert = le32(c + 0x08);
    t->ticket_size = le32(c + 0x0C);
    t->tmd_size = le32(c + 0x10);
    t->meta_size = le32(c + 0x14);
    t->content_size = (size_t)le64(c + 0x18);
    t->ticket = align_up(align_up(0x2020, 64) + cert, 64);
    t->tmd = align_up(t->ticket + t->ticket_size, 64);
    t->content = align_up(t->tmd + t->tmd_size, 64);
    t->meta = align_up(t->content + t->content_size, 64);
    if (t->meta + t->meta_size > t->size) return set_error(error, error_size, "shortcut template cut short");
    const uint8_t *ncch = c + t->content;
    if (memcmp(ncch + 0x100, "NCCH", 4)) return set_error(error, error_size, "shortcut template has no NCCH");
    /* The ExeFS must be the last thing in it (no RomFS), so it can grow. */
    if (le32(ncch + 0x1B4)) return set_error(error, error_size, "shortcut template has a RomFS");
    t->exefs = (size_t)le32(ncch + 0x1A0) * MEDIA_UNIT;
    const size_t exefs_size = (size_t)le32(ncch + 0x1A4) * MEDIA_UNIT;
    if (t->exefs + exefs_size != t->content_size) return set_error(error, error_size, "shortcut template layout");
    const uint8_t *exefs = ncch + t->exefs;
    for (int i = 0; i < EXEFS_FILES; ++i) {
        const uint8_t *entry = exefs + i * 16;
        const uint32_t offset = le32(entry + 8), size = le32(entry + 12);
        if (!size) continue;
        if (MEDIA_UNIT + (size_t)offset + size > exefs_size) return set_error(error, error_size, "shortcut template ExeFS");
        memcpy(t->files[i].name, entry, 8);
        t->files[i].data = exefs + MEDIA_UNIT + offset;
        t->files[i].size = size;
    }
    return true;
}

static int find_file(const Template *t, const char *name)
{
    for (int i = 0; i < EXEFS_FILES; ++i)
        if (t->files[i].size && !strncmp(t->files[i].name, name, 8)) return i;
    return -1;
}

bool shortcut_cia_template_smdh(uint8_t smdh[SHORTCUT_SMDH_SIZE])
{
    Template t;
    if (!parse_template(&t, NULL, 0)) return false;
    const int icon = find_file(&t, "icon");
    if (icon < 0 || t.files[icon].size != SHORTCUT_SMDH_SIZE) return false;
    memcpy(smdh, t.files[icon].data, SHORTCUT_SMDH_SIZE);
    return memcmp(smdh, "SMDH", 4) == 0;
}

/* LZ11 as the banner format wants it (the HOME Menu unpacks it). */
static uint8_t *lz11_unpack(const uint8_t *in, size_t in_size, size_t *out_size)
{
    if (in_size < 4 || in[0] != 0x11) return NULL;
    const size_t size = in[1] | in[2] << 8 | (size_t)in[3] << 16;
    uint8_t *out = malloc(size ? size : 1);
    if (!out) return NULL;
    size_t i = 4, o = 0;
    while (o < size) {
        if (i >= in_size) { free(out); return NULL; }
        const uint8_t flags = in[i++];
        for (int bit = 0; bit < 8 && o < size; ++bit) {
            if (!(flags & (0x80 >> bit))) {
                if (i >= in_size) { free(out); return NULL; }
                out[o++] = in[i++];
                continue;
            }
            if (i + 1 >= in_size) { free(out); return NULL; }
            const uint8_t b0 = in[i];
            size_t count, distance;
            switch (b0 >> 4) {
            case 0:
                if (i + 2 >= in_size) { free(out); return NULL; }
                count = (size_t)(((b0 & 0xF) << 4) | (in[i + 1] >> 4)) + 0x11;
                distance = (size_t)(((in[i + 1] & 0xF) << 8) | in[i + 2]) + 1;
                i += 3;
                break;
            case 1:
                if (i + 3 >= in_size) { free(out); return NULL; }
                count = (size_t)(((b0 & 0xF) << 12) | (in[i + 1] << 4) | (in[i + 2] >> 4)) + 0x111;
                distance = (size_t)(((in[i + 2] & 0xF) << 8) | in[i + 3]) + 1;
                i += 4;
                break;
            default:
                count = (size_t)(b0 >> 4) + 1;
                distance = (size_t)(((b0 & 0xF) << 8) | in[i + 1]) + 1;
                i += 2;
                break;
            }
            if (distance > o) { free(out); return NULL; }
            for (size_t n = 0; n < count && o < size; ++n, ++o) out[o] = out[o - distance];
        }
    }
    *out_size = size;
    return out;
}

/* Stored, not compressed: every block flagged literal. Valid LZ11 that
 * needs no search on the console; the banner grows ~70 KB. */
static size_t lz11_store(const uint8_t *in, size_t size, uint8_t *out)
{
    size_t o = 0;
    out[o++] = 0x11;
    out[o++] = (uint8_t)size;
    out[o++] = (uint8_t)(size >> 8);
    out[o++] = (uint8_t)(size >> 16);
    for (size_t i = 0; i < size; i += 8) {
        out[o++] = 0;
        const size_t n = size - i < 8 ? size - i : 8;
        memcpy(out + o, in + i, n);
        o += n;
    }
    return o;
}

/* CBMD header, then the CGFX (with the new texture), then the template's
 * CWAV (the koto sound), as bannertool lays it out. */
static uint8_t *build_banner(const uint8_t *banner, size_t banner_size, const uint16_t *texture,
                             size_t *out_size, char *error, size_t error_size)
{
    if (banner_size < 0x88 || memcmp(banner, "CBMD", 4)) {
        set_error(error, error_size, "shortcut banner template");
        return NULL;
    }
    const size_t cgfx_offset = le32(banner + 0x08), cwav_offset = le32(banner + 0x84);
    if (cgfx_offset < 0x88 || cgfx_offset >= banner_size || cwav_offset + 0x10 > banner_size) {
        set_error(error, error_size, "shortcut banner layout");
        return NULL;
    }
    size_t cgfx_size = 0;
    uint8_t *cgfx = lz11_unpack(banner + cgfx_offset, banner_size - cgfx_offset, &cgfx_size);
    if (!cgfx || cgfx_size != CGFX_SIZE || memcmp(cgfx, "CGFX", 4)) {
        free(cgfx);
        set_error(error, error_size, "shortcut banner model");
        return NULL;
    }
    memcpy(cgfx + CGFX_TEXTURE_OFFSET, texture, CGFX_TEXTURE_SIZE);
    const uint8_t *cwav = banner + cwav_offset;
    const size_t cwav_size = le32(cwav + 0x0C);
    if (memcmp(cwav, "CWAV", 4) || cwav_offset + cwav_size > banner_size) {
        free(cgfx);
        set_error(error, error_size, "shortcut banner sound");
        return NULL;
    }
    const size_t packed_max = 4 + cgfx_size + (cgfx_size + 7) / 8;
    const size_t total_max = align_up(cgfx_offset + packed_max, 0x20) + cwav_size;
    uint8_t *out = calloc(1, total_max);
    if (!out) {
        free(cgfx);
        set_error(error, error_size, "out of memory");
        return NULL;
    }
    memcpy(out, banner, cgfx_offset);
    const size_t packed = lz11_store(cgfx, cgfx_size, out + cgfx_offset);
    free(cgfx);
    const size_t new_cwav = align_up(cgfx_offset + packed, 0x20);
    memcpy(out + new_cwav, cwav, cwav_size);
    put_le32(out + 0x84, (uint32_t)new_cwav);
    *out_size = new_cwav + cwav_size;
    return out;
}

uint8_t *shortcut_cia_build(uint64_t title_id, const uint8_t smdh[SHORTCUT_SMDH_SIZE],
                            const uint16_t *banner_texture, size_t *size, char *error, size_t error_size)
{
    Template t;
    if (!parse_template(&t, error, error_size)) return NULL;
    const int banner_index = find_file(&t, "banner"), icon_index = find_file(&t, "icon");
    if (banner_index < 0 || icon_index < 0 || t.files[icon_index].size != SHORTCUT_SMDH_SIZE) {
        set_error(error, error_size, "shortcut template has no icon or banner");
        return NULL;
    }
    size_t banner_size = 0;
    uint8_t *banner = build_banner(t.files[banner_index].data, t.files[banner_index].size, banner_texture,
                                   &banner_size, error, error_size);
    if (!banner) return NULL;

    /* The new ExeFS: same files in the same order, banner and icon replaced. */
    const uint8_t *file_data[EXEFS_FILES];
    size_t file_size[EXEFS_FILES];
    size_t exefs_size = MEDIA_UNIT;
    for (int i = 0; i < EXEFS_FILES; ++i) {
        file_data[i] = i == banner_index ? banner : i == icon_index ? smdh : t.files[i].data;
        file_size[i] = i == banner_index ? banner_size : t.files[i].size;
        if (file_size[i]) exefs_size += align_up(file_size[i], MEDIA_UNIT);
    }
    const size_t ncch_size = t.exefs + exefs_size;
    const size_t content = t.content;
    const size_t meta = align_up(content + ncch_size, 64);
    const size_t total = meta + t.meta_size;
    uint8_t *cia = calloc(1, total);
    if (!cia) {
        free(banner);
        set_error(error, error_size, "out of memory");
        return NULL;
    }
    /* Header, certificates, ticket, TMD and the NCCH up to its ExeFS. */
    memcpy(cia, t.data, content + t.exefs);
    uint8_t *ncch = cia + content;
    uint8_t *exefs = ncch + t.exefs;
    size_t at = 0;
    for (int i = 0; i < EXEFS_FILES; ++i) {
        if (!file_size[i]) continue;
        uint8_t *entry = exefs + i * 16;
        memcpy(entry, t.files[i].name, 8);
        put_le32(entry + 8, (uint32_t)at);
        put_le32(entry + 12, (uint32_t)file_size[i]);
        memcpy(exefs + MEDIA_UNIT + at, file_data[i], file_size[i]);
        /* Hashes are stored last file first. */
        sha256(file_data[i], file_size[i], exefs + MEDIA_UNIT - (size_t)(i + 1) * 32);
        at += align_up(file_size[i], MEDIA_UNIT);
    }
    free(banner);

    /* The title ID everywhere the NCCH and its extended header carry it. */
    static const size_t id_offsets[] = { 0x108, 0x118, 0x200 + 0x1C8, 0x200 + 0x200, 0x200 + 0x600 };
    for (size_t i = 0; i < sizeof(id_offsets) / sizeof(id_offsets[0]); ++i) put_le64(ncch + id_offsets[i], title_id);
    sha256(ncch + 0x200, 0x400, ncch + 0x160);
    put_le32(ncch + 0x104, (uint32_t)(ncch_size / MEDIA_UNIT));
    put_le32(ncch + 0x1A4, (uint32_t)(exefs_size / MEDIA_UNIT));
    const size_t hashed = (size_t)le32(ncch + 0x1A8) * MEDIA_UNIT;
    sha256(exefs, hashed ? hashed : MEDIA_UNIT, ncch + 0x1C0);

    /* CIA header, ticket and TMD. */
    put_le64(cia + 0x18, ncch_size);
    const size_t ticket = t.ticket + signature_size(be32(cia + t.ticket));
    const size_t tmd = t.tmd + signature_size(be32(cia + t.tmd));
    if (ticket == t.ticket || tmd == t.tmd) {
        free(cia);
        set_error(error, error_size, "shortcut template signature type");
        return NULL;
    }
    put_be64(cia + ticket + 0x9C, title_id);
    put_be64(cia + tmd + 0x4C, title_id);
    const unsigned count = be16(cia + tmd + 0x9E);
    uint8_t *infos = cia + tmd + 0xC4;
    uint8_t *chunk = infos + 64 * 0x24;
    if (count != 1) {
        free(cia);
        set_error(error, error_size, "shortcut template has %u contents", count);
        return NULL;
    }
    put_be64(chunk + 8, ncch_size);
    sha256(ncch, ncch_size, chunk + 0x10);
    sha256(chunk, 0x30 * count, infos + 4);
    sha256(infos, 64 * 0x24, cia + tmd + 0xA4);

    /* Meta: FBI and the like show this copy of the icon. */
    memcpy(cia + meta, t.data + t.meta, t.meta_size);
    if (t.meta_size >= 0x400 + SHORTCUT_SMDH_SIZE) memcpy(cia + meta + 0x400, smdh, SHORTCUT_SMDH_SIZE);
    *size = total;
    return cia;
}
