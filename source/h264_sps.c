#include "h264_sps.h"

#include <string.h>

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t bit;
    bool error;
} BitReader;

static unsigned read_bits(BitReader *r, unsigned count)
{
    if (count > 32 || r->bit + count > r->size * 8) {
        r->error = true;
        return 0;
    }
    unsigned value = 0;
    while (count--) {
        value = (value << 1) | ((r->data[r->bit >> 3] >> (7 - (r->bit & 7))) & 1);
        r->bit++;
    }
    return value;
}

static unsigned read_ue(BitReader *r)
{
    unsigned zeros = 0;
    while (!r->error && read_bits(r, 1) == 0 && zeros < 31) zeros++;
    if (r->error || zeros >= 31) {
        r->error = true;
        return 0;
    }
    return ((1u << zeros) - 1u) + (zeros ? read_bits(r, zeros) : 0);
}

static int read_se(BitReader *r)
{
    unsigned value = read_ue(r);
    return (value & 1) ? (int)((value + 1) >> 1) : -(int)(value >> 1);
}

static void skip_scaling_list(BitReader *r, unsigned count)
{
    int last = 8, next = 8;
    for (unsigned i = 0; i < count && !r->error; ++i) {
        if (next) next = (last + read_se(r) + 256) & 255;
        if (next) last = next;
    }
}

static bool parse_sps(const uint8_t *ebsp, size_t ebsp_size,
                      unsigned *width, unsigned *height,
                      unsigned *profile, unsigned *level, unsigned *refs)
{
    uint8_t rbsp[512];
    size_t rbsp_size = 0;
    unsigned zero_count = 0;
    for (size_t i = 0; i < ebsp_size && rbsp_size < sizeof(rbsp); ++i) {
        if (zero_count >= 2 && ebsp[i] == 3) {
            zero_count = 0;
            continue;
        }
        rbsp[rbsp_size++] = ebsp[i];
        zero_count = ebsp[i] == 0 ? zero_count + 1 : 0;
    }
    BitReader r = { rbsp, rbsp_size, 0, false };
    unsigned profile_idc = read_bits(&r, 8);
    read_bits(&r, 8);
    unsigned level_idc = read_bits(&r, 8);
    read_ue(&r);
    unsigned chroma_format_idc = 1;
    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
        profile_idc == 244 || profile_idc == 44 || profile_idc == 83 ||
        profile_idc == 86 || profile_idc == 118 || profile_idc == 128 ||
        profile_idc == 138 || profile_idc == 139 || profile_idc == 134 ||
        profile_idc == 135) {
        chroma_format_idc = read_ue(&r);
        if (chroma_format_idc == 3) read_bits(&r, 1);
        read_ue(&r);
        read_ue(&r);
        read_bits(&r, 1);
        if (read_bits(&r, 1)) {
            unsigned lists = chroma_format_idc == 3 ? 12 : 8;
            for (unsigned i = 0; i < lists; ++i)
                if (read_bits(&r, 1)) skip_scaling_list(&r, i < 6 ? 16 : 64);
        }
    }
    read_ue(&r);
    unsigned poc_type = read_ue(&r);
    if (poc_type == 0) read_ue(&r);
    else if (poc_type == 1) {
        read_bits(&r, 1);
        read_se(&r);
        read_se(&r);
        unsigned cycle = read_ue(&r);
        for (unsigned i = 0; i < cycle; ++i) read_se(&r);
    }
    unsigned max_num_ref_frames = read_ue(&r);
    read_bits(&r, 1);
    unsigned width_mbs = read_ue(&r) + 1;
    unsigned height_map_units = read_ue(&r) + 1;
    unsigned frame_mbs_only = read_bits(&r, 1);
    if (!frame_mbs_only) read_bits(&r, 1);
    read_bits(&r, 1);
    unsigned crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
    if (read_bits(&r, 1)) {
        crop_left = read_ue(&r); crop_right = read_ue(&r);
        crop_top = read_ue(&r); crop_bottom = read_ue(&r);
    }
    if (r.error || chroma_format_idc > 3) return false;
    static const unsigned sub_width[] = { 1, 2, 2, 1 };
    static const unsigned sub_height[] = { 1, 2, 1, 1 };
    unsigned crop_x = sub_width[chroma_format_idc];
    unsigned crop_y = sub_height[chroma_format_idc] * (2 - frame_mbs_only);
    unsigned coded_width = width_mbs * 16;
    unsigned coded_height = height_map_units * 16 * (2 - frame_mbs_only);
    unsigned crop_w = crop_x * (crop_left + crop_right);
    unsigned crop_h = crop_y * (crop_top + crop_bottom);
    if (crop_w >= coded_width || crop_h >= coded_height) return false;
    *width = coded_width - crop_w;
    *height = coded_height - crop_h;
    *profile = profile_idc;
    *level = level_idc;
    *refs = max_num_ref_frames;
    return true;
}

bool h264_sps_dimensions(const uint8_t *data, size_t size,
                         unsigned *width, unsigned *height,
                         unsigned *profile, unsigned *level, unsigned *refs,
                         uint32_t *signature)
{
    for (size_t i = 0; i + 5 < size; ++i) {
        size_t header = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) header = i + 3;
        else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
            header = i + 4;
        if (!header || (data[header] & 0x1f) != 7) continue;
        size_t end = header + 1;
        while (end + 3 < size && !(data[end] == 0 && data[end + 1] == 0 &&
               (data[end + 2] == 1 || (data[end + 2] == 0 && data[end + 3] == 1)))) end++;
        uint32_t hash = 2166136261u;
        for (size_t j = header; j < end; ++j) hash = (hash ^ data[j]) * 16777619u;
        if (!parse_sps(data + header + 1, end - header - 1,
                       width, height, profile, level, refs)) return false;
        *signature = hash;
        return true;
    }
    return false;
}
