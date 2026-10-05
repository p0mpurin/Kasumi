#include "qr.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Error correction level M: EC codewords per block, blocks, total codewords. */
static const int EC_PER_BLOCK[7] = { 0, 10, 16, 26, 18, 24, 16 };
static const int BLOCKS[7] = { 0, 1, 1, 1, 2, 2, 4 };
static const int TOTAL[7] = { 0, 26, 44, 70, 100, 134, 172 };

static int gf_mul(int a, int b)
{
    int result = 0;
    while (b) {
        if (b & 1) result ^= a;
        a <<= 1;
        if (a & 0x100) a ^= 0x11D;
        b >>= 1;
    }
    return result;
}

/* Reed-Solomon generator polynomial of a degree (degree + 1 terms). */
static void rs_generator(int degree, int *poly)
{
    int length = 1, root = 1;
    poly[0] = 1;
    for (int d = 0; d < degree; ++d) {
        int next[32];
        for (int i = 0; i <= length; ++i) {
            const int c = i < length ? poly[i] : 0;
            const int n = i > 0 ? poly[i - 1] : 0;
            next[i] = c ^ gf_mul(n, root);
        }
        ++length;
        memcpy(poly, next, (size_t)length * sizeof(int));
        root = gf_mul(root, 2);
    }
}

static void rs_remainder(const unsigned char *data, int length, int degree, unsigned char *rem)
{
    int gen[32];
    rs_generator(degree, gen);
    memset(rem, 0, (size_t)degree);
    for (int k = 0; k < length; ++k) {
        const int factor = data[k] ^ rem[0];
        memmove(rem, rem + 1, (size_t)degree - 1);
        rem[degree - 1] = 0;
        for (int i = 0; i < degree; ++i) rem[i] ^= (unsigned char)gf_mul(gen[i + 1], factor);
    }
}

/* Data and error correction codewords, interleaved; returns how many. */
static int codewords(const unsigned char *payload, int length, unsigned char *out, int *version_out)
{
    int version, data_len = 0;
    for (version = 1; version <= 6; ++version) {
        data_len = TOTAL[version] - EC_PER_BLOCK[version] * BLOCKS[version];
        if (length + 2 <= data_len) break; /* 4-bit mode + 8-bit count + data */
    }
    if (version > 6) return 0;
    unsigned char data[172];
    memset(data, 0, sizeof(data));
    int bit = 0;
#define PUT(value, bits) \
    for (int k = (bits) - 1; k >= 0; --k, ++bit) \
        if (((value) >> k) & 1) data[bit >> 3] |= (unsigned char)(0x80 >> (bit & 7))
    PUT(4, 4);
    PUT(length, 8);
    for (int i = 0; i < length; ++i) PUT(payload[i], 8);
#undef PUT
    const int capacity = data_len * 8;
    bit += capacity - bit < 4 ? capacity - bit : 4;
    bit = (bit + 7) / 8 * 8;
    int count = bit / 8;
    for (int pad = 0xEC; count < data_len; pad ^= 0xEC ^ 0x11) data[count++] = (unsigned char)pad;

    const int blocks = BLOCKS[version], ec = EC_PER_BLOCK[version], size = data_len / blocks;
    unsigned char ecc[4][32];
    for (int b = 0; b < blocks; ++b) rs_remainder(data + b * size, size, ec, ecc[b]);
    int n = 0;
    for (int i = 0; i < size; ++i)
        for (int b = 0; b < blocks; ++b) out[n++] = data[b * size + i];
    for (int i = 0; i < ec; ++i)
        for (int b = 0; b < blocks; ++b) out[n++] = ecc[b][i];
    *version_out = version;
    return n;
}

static bool mask_bit(int mask, int x, int y)
{
    switch (mask) {
    case 0: return (x + y) % 2 == 0;
    case 1: return y % 2 == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return (x / 3 + y / 2) % 2 == 0;
    case 5: return x * y % 2 + x * y % 3 == 0;
    case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
    default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

static void apply_mask(const unsigned char *modules, const unsigned char *function, int n, int mask,
                       unsigned char *grid)
{
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const int i = y * n + x;
            grid[i] = modules[i];
            if (!function[i] && mask_bit(mask, x, y)) grid[i] ^= 1;
        }
    /* Format information: level M (0) and the mask, BCH-coded. */
    const int data = (0 << 3) | mask;
    int rem = data;
    for (int k = 0; k < 10; ++k) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    const int bits = ((data << 10) | rem) ^ 0x5412;
#define BIT(i) ((unsigned char)((bits >> (i)) & 1))
#define FMT(x, y, v) grid[(y) * n + (x)] = (v)
    for (int i = 0; i < 6; ++i) FMT(8, i, BIT(i));
    FMT(8, 7, BIT(6));
    FMT(8, 8, BIT(7));
    FMT(7, 8, BIT(8));
    for (int i = 9; i < 15; ++i) FMT(14 - i, 8, BIT(i));
    for (int i = 0; i < 8; ++i) FMT(n - 1 - i, 8, BIT(i));
    for (int i = 8; i < 15; ++i) FMT(8, n - 15 + i, BIT(i));
    FMT(8, n - 8, 1);
#undef FMT
#undef BIT
}

/* Non-overlapping occurrences of an 11-module pattern in a line. */
static int count_pattern(const unsigned char *line, int n, const char *pattern)
{
    int found = 0;
    for (int i = 0; i + 11 <= n;) {
        int k = 0;
        while (k < 11 && line[i + k] == (unsigned char)(pattern[k] - '0')) ++k;
        if (k == 11) { ++found; i += 11; }
        else ++i;
    }
    return found;
}

static int penalty(const unsigned char *grid, int n)
{
    int score = 0;
    unsigned char line[QR_MAX_SIZE];
    for (int pass = 0; pass < 2; ++pass) {
        for (int a = 0; a < n; ++a) {
            for (int b = 0; b < n; ++b) line[b] = pass ? grid[b * n + a] : grid[a * n + b];
            int run = 0, colour = -1;
            for (int b = 0; b < n; ++b) {
                if (line[b] == colour) {
                    ++run;
                } else {
                    if (run >= 5) score += run - 2;
                    run = 1;
                    colour = line[b];
                }
            }
            if (run >= 5) score += run - 2;
            score += 40 * (count_pattern(line, n, "10111010000") + count_pattern(line, n, "00001011101"));
        }
    }
    for (int y = 0; y < n - 1; ++y)
        for (int x = 0; x < n - 1; ++x) {
            const unsigned char c = grid[y * n + x];
            if (c == grid[y * n + x + 1] && c == grid[(y + 1) * n + x] && c == grid[(y + 1) * n + x + 1]) score += 3;
        }
    int dark = 0;
    for (int i = 0; i < n * n; ++i) dark += grid[i];
    score += abs(dark * 20 - n * n * 10) / (n * n) * 10;
    return score;
}

int qr_encode(const char *text, unsigned char out[QR_MAX_SIZE * QR_MAX_SIZE])
{
    unsigned char words[172];
    int version = 0;
    const int length = (int)strlen(text);
    if (length > 106) return 0;
    const int word_count = codewords((const unsigned char *)text, length, words, &version);
    if (!word_count) return 0;
    const int n = version * 4 + 17;
    static unsigned char modules[QR_MAX_SIZE * QR_MAX_SIZE], function[QR_MAX_SIZE * QR_MAX_SIZE];
    static unsigned char grid[QR_MAX_SIZE * QR_MAX_SIZE];
    memset(modules, 0, sizeof(modules));
    memset(function, 0, sizeof(function));
#define PUT(x, y, dark) do { modules[(y) * n + (x)] = (unsigned char)(dark); function[(y) * n + (x)] = 1; } while (0)
    for (int i = 0; i < n; ++i) { /* timing patterns */
        PUT(6, i, i % 2 == 0);
        PUT(i, 6, i % 2 == 0);
    }
    const int centres[3][2] = { { 3, 3 }, { n - 4, 3 }, { 3, n - 4 } };
    for (int f = 0; f < 3; ++f) /* finders + separators */
        for (int dy = -4; dy <= 4; ++dy)
            for (int dx = -4; dx <= 4; ++dx) {
                const int x = centres[f][0] + dx, y = centres[f][1] + dy;
                if (x < 0 || x >= n || y < 0 || y >= n) continue;
                const int dist = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
                PUT(x, y, dist != 2 && dist != 4);
            }
    if (version >= 2) { /* one alignment pattern for versions 2-6 */
        const int c = n - 7;
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                const int dist = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
                PUT(c + dx, c + dy, dist != 1);
            }
    }
    /* Reserve the format areas (drawn per mask) and the dark module. */
    for (int i = 0; i < 9; ++i) function[8 * n + i] = function[i * n + 8] = 1;
    for (int i = 0; i < 8; ++i) function[8 * n + n - 1 - i] = function[(n - 1 - i) * n + 8] = 1;
    PUT(8, n - 8, 1);
#undef PUT

    /* Data, in the standard zig-zag from the bottom-right corner. */
    int bit_index = 0;
    const int total_bits = word_count * 8;
    for (int right = n - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < n; ++vert)
            for (int j = 0; j < 2; ++j) {
                const int x = right - j;
                const bool upward = ((right + 1) & 2) == 0;
                const int y = upward ? n - 1 - vert : vert;
                if (!function[y * n + x] && bit_index < total_bits) {
                    modules[y * n + x] = (words[bit_index >> 3] >> (7 - (bit_index & 7))) & 1;
                    ++bit_index;
                }
            }
    }

    int best = -1, best_score = 0;
    for (int mask = 0; mask < 8; ++mask) {
        apply_mask(modules, function, n, mask, grid);
        const int score = penalty(grid, n);
        if (best < 0 || score < best_score) { best = mask; best_score = score; }
    }
    apply_mask(modules, function, n, best, out);
    return n;
}
