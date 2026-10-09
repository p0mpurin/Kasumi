#include "steam_crypto.h"

#include <mbedtls/aes.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha256.h>
#include <string.h>

/* Valve's key for the public Steam universe: pairing requests carry a key
 * escrow ticket encrypted with it (from ihslib's client/pubkeys.h). */
static const uint8_t VALVE_PUBLIC_KEY[] = {
    0x30, 0x82, 0x01, 0x20, 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01, 0x05,
    0x00, 0x03, 0x82, 0x01, 0x0d, 0x00, 0x30, 0x82, 0x01, 0x08, 0x02, 0x82, 0x01, 0x01, 0x00, 0xc7, 0xb8, 0x90,
    0x2e, 0xa4, 0x24, 0x9b, 0x5e, 0xe1, 0x24, 0xfa, 0x46, 0x88, 0xea, 0x5b, 0xfd, 0x02, 0x47, 0xb5, 0x0a, 0x62,
    0x2a, 0x4d, 0xa9, 0x9b, 0x5c, 0xe8, 0x9a, 0x2b, 0xaa, 0xb3, 0x74, 0xb8, 0x6f, 0x03, 0xab, 0xa1, 0x9f, 0xa6,
    0x5f, 0xdc, 0x86, 0xca, 0xce, 0x46, 0xa4, 0x4e, 0xb5, 0xab, 0x42, 0x1a, 0x69, 0xd1, 0x03, 0x78, 0xc3, 0xb8,
    0x67, 0x23, 0xdf, 0xc2, 0x39, 0x5b, 0xea, 0x4a, 0xb3, 0x9d, 0xab, 0x91, 0x27, 0x61, 0xe3, 0xe3, 0x43, 0xab,
    0x98, 0x24, 0xb0, 0x45, 0x6e, 0x57, 0xda, 0xba, 0xde, 0xfd, 0xa4, 0xd1, 0xc1, 0xc7, 0xab, 0x28, 0x5e, 0x64,
    0x7d, 0xb1, 0x18, 0x8b, 0xcc, 0xf8, 0xef, 0xe5, 0x8a, 0x75, 0x95, 0xcc, 0xdd, 0x31, 0x84, 0x5c, 0x0c, 0xf7,
    0x5b, 0x8a, 0xb4, 0xb2, 0x70, 0x7d, 0x21, 0x8e, 0xf6, 0xd4, 0x6a, 0x11, 0x5d, 0x08, 0x5f, 0xcc, 0x75, 0x4a,
    0x75, 0xc5, 0x0d, 0xd8, 0xd0, 0x18, 0x93, 0x6c, 0xf5, 0xe9, 0x64, 0x2c, 0x66, 0x45, 0x63, 0x7e, 0xc3, 0xb5,
    0x09, 0x48, 0x11, 0xb9, 0xce, 0xed, 0xb1, 0x7d, 0x23, 0x47, 0x81, 0xce, 0x54, 0x45, 0xc2, 0xdd, 0xb3, 0x8a,
    0x78, 0x7c, 0x95, 0x85, 0x12, 0x16, 0x7d, 0x7a, 0x2f, 0x3f, 0x34, 0x79, 0xac, 0x27, 0x21, 0xaa, 0xdd, 0x98,
    0x11, 0x9e, 0xe8, 0x32, 0xf3, 0x21, 0xc6, 0x29, 0xc6, 0x57, 0x24, 0x34, 0xa2, 0x42, 0xf5, 0xaf, 0xf5, 0x13,
    0x07, 0xce, 0xda, 0x08, 0x23, 0x2e, 0xf5, 0x64, 0x60, 0xd4, 0xf1, 0x91, 0xc8, 0xf4, 0x88, 0x4a, 0x3e, 0x2a,
    0x14, 0x36, 0x17, 0x10, 0x0c, 0x43, 0xcf, 0x55, 0xad, 0x1f, 0xc7, 0xc7, 0x05, 0x20, 0x7d, 0x86, 0x86, 0x97,
    0xcc, 0x9b, 0x23, 0xde, 0xed, 0x46, 0x07, 0x63, 0x2f, 0x7e, 0xa5, 0x4d, 0x49, 0x89, 0x83, 0x3d, 0xc9, 0x20,
    0xe7, 0x02, 0x01, 0x11
};

typedef struct {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
} Rng;

static bool rng_open(Rng *r)
{
    mbedtls_entropy_init(&r->entropy);
    mbedtls_ctr_drbg_init(&r->drbg);
    static const unsigned char personal[] = "Kasumi Steam Link";
    return mbedtls_ctr_drbg_seed(&r->drbg, mbedtls_entropy_func, &r->entropy, personal, sizeof(personal) - 1) == 0;
}

static void rng_close(Rng *r)
{
    mbedtls_ctr_drbg_free(&r->drbg);
    mbedtls_entropy_free(&r->entropy);
}

bool steam_random(void *out, size_t size)
{
    Rng r;
    const bool ok = rng_open(&r) && mbedtls_ctr_drbg_random(&r.drbg, out, size) == 0;
    rng_close(&r);
    return ok;
}

static bool aes_ecb_block(const uint8_t *key, size_t key_size, const uint8_t in[16], uint8_t out[16], bool encrypt)
{
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    const int set = encrypt ? mbedtls_aes_setkey_enc(&aes, key, (unsigned)key_size * 8)
                            : mbedtls_aes_setkey_dec(&aes, key, (unsigned)key_size * 8);
    const bool ok = set == 0 &&
                    mbedtls_aes_crypt_ecb(&aes, encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT, in, out) == 0;
    mbedtls_aes_free(&aes);
    return ok;
}

/* AES-CBC of `in` plus PKCS#7 padding; returns the ciphertext length or 0. */
static size_t cbc_encrypt(const uint8_t *key, size_t key_size, const uint8_t iv_in[16], const uint8_t *in,
                          size_t size, uint8_t *out, size_t out_cap)
{
    const size_t total = (size / 16 + 1) * 16;
    if (total > out_cap) return 0;
    memcpy(out, in, size);
    memset(out + size, (int)(total - size), total - size);
    uint8_t iv[16];
    memcpy(iv, iv_in, 16);
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    const bool ok = mbedtls_aes_setkey_enc(&aes, key, (unsigned)key_size * 8) == 0 &&
                    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, total, iv, out, out) == 0;
    mbedtls_aes_free(&aes);
    return ok ? total : 0;
}

int steam_cbc_decrypt(const uint8_t *key, size_t key_size, const uint8_t iv_in[16], const uint8_t *in,
                      size_t size, uint8_t *out, size_t out_cap)
{
    if (!size || size % 16 || size > out_cap) return -1;
    uint8_t iv[16];
    memcpy(iv, iv_in, 16);
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    const bool ok = mbedtls_aes_setkey_dec(&aes, key, (unsigned)key_size * 8) == 0 &&
                    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, size, iv, in, out) == 0;
    mbedtls_aes_free(&aes);
    if (!ok) return -1;
    const uint8_t pad = out[size - 1];
    if (!pad || pad > 16) return -1;
    for (size_t i = size - pad; i < size; ++i)
        if (out[i] != pad) return -1;
    return (int)(size - pad);
}

size_t steam_sym_encrypt(const uint8_t *key, size_t key_size, const void *in, size_t size, uint8_t *out,
                         size_t out_cap)
{
    uint8_t iv[16];
    if (out_cap < 16 || !steam_random(iv, sizeof(iv)) || !aes_ecb_block(key, key_size, iv, out, true)) return 0;
    const size_t body = cbc_encrypt(key, key_size, iv, in, size, out + 16, out_cap - 16);
    return body ? 16 + body : 0;
}

int steam_sym_decrypt(const uint8_t *key, size_t key_size, const uint8_t *in, size_t size, uint8_t *out,
                      size_t out_cap)
{
    uint8_t iv[16];
    if (size < 32 || !aes_ecb_block(key, key_size, in, iv, false)) return -1;
    return steam_cbc_decrypt(key, key_size, iv, in + 16, size - 16, out, out_cap);
}

static bool hmac(mbedtls_md_type_t type, const uint8_t *key, size_t key_size, const void *in, size_t size,
                 uint8_t *out)
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(type);
    return md && mbedtls_md_hmac(md, key, key_size, in, size, out) == 0;
}

size_t steam_frame_encrypt(const uint8_t *key, size_t key_size, uint64_t sequence, const void *in, size_t size,
                           uint8_t *out, size_t out_cap)
{
    if (out_cap < 16 + size + 8 + 16) return 0;
    /* The plaintext is built after the IV's room, then encrypted in place. */
    uint8_t *plain = out + 16;
    for (unsigned i = 0; i < 8; ++i) plain[i] = (uint8_t)(sequence >> (8 * i));
    memcpy(plain + 8, in, size);
    if (!hmac(MBEDTLS_MD_MD5, key, key_size, plain, size + 8, out)) return 0;
    const size_t body = cbc_encrypt(key, key_size, out, plain, size + 8, plain, out_cap - 16);
    return body ? 16 + body : 0;
}

int steam_frame_decrypt(const uint8_t *key, size_t key_size, const uint8_t *in, size_t size, uint8_t *out,
                        size_t out_cap, uint64_t *sequence, const uint8_t **message)
{
    if (size < 32) return -1;
    const int n = steam_cbc_decrypt(key, key_size, in, in + 16, size - 16, out, out_cap);
    if (n < 8) return -1;
    uint8_t check[16];
    if (!hmac(MBEDTLS_MD_MD5, key, key_size, out, (size_t)n, check) || memcmp(check, in, 16)) return -1;
    uint64_t seq = 0;
    for (unsigned i = 0; i < 8; ++i) seq |= (uint64_t)out[i] << (8 * i);
    *sequence = seq;
    *message = out + 8;
    return n - 8;
}

bool steam_hmac_sha256(const uint8_t *key, size_t key_size, const void *in, size_t size, uint8_t out[32])
{
    return hmac(MBEDTLS_MD_SHA256, key, key_size, in, size, out);
}

bool steam_sha256(const void *in, size_t size, uint8_t out[32])
{
    return mbedtls_sha256_ret(in, size, out, 0) == 0;
}

/* X25519 on mbedTLS' Montgomery curve: little-endian X-only, as RFC 7748.
 * peer == NULL multiplies the base point (the public key). */
static bool x25519(const uint8_t private_key[32], const uint8_t *peer, uint8_t out[32])
{
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point point, result;
    Rng r;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&point);
    mbedtls_ecp_point_init(&result);
    uint8_t scalar[32];
    memcpy(scalar, private_key, 32);
    scalar[0] &= 248;
    scalar[31] &= 127;
    scalar[31] |= 64;
    size_t length = 0;
    bool ok = rng_open(&r) && mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) == 0 &&
              mbedtls_mpi_read_binary_le(&d, scalar, sizeof(scalar)) == 0 &&
              (!peer || mbedtls_ecp_point_read_binary(&grp, &point, peer, 32) == 0) &&
              mbedtls_ecp_mul(&grp, &result, &d, peer ? &point : &grp.G, mbedtls_ctr_drbg_random, &r.drbg) == 0 &&
              mbedtls_ecp_point_write_binary(&grp, &result, MBEDTLS_ECP_PF_UNCOMPRESSED, &length, out, 32) == 0 &&
              length == 32;
    /* A low-order peer key gives all zeros (RFC 7748 says to refuse it). */
    uint8_t any = 0;
    for (unsigned i = 0; ok && i < 32; ++i) any |= out[i];
    ok = ok && any;
    memset(scalar, 0, sizeof(scalar));
    rng_close(&r);
    mbedtls_ecp_point_free(&result);
    mbedtls_ecp_point_free(&point);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

bool steam_x25519_keypair(uint8_t private_key[32], uint8_t public_key[32])
{
    return steam_random(private_key, 32) && x25519(private_key, NULL, public_key);
}

bool steam_key_exchange(const uint8_t private_key[32], const uint8_t peer_public[32], uint8_t secret[32])
{
    uint8_t shared[32];
    const bool ok = x25519(private_key, peer_public, shared) && steam_sha256(shared, sizeof(shared), secret);
    memset(shared, 0, sizeof(shared));
    return ok;
}

size_t steam_rsa_encrypt(const void *in, size_t size, uint8_t *out, size_t out_cap)
{
    mbedtls_pk_context pk;
    Rng r;
    mbedtls_pk_init(&pk);
    size_t result = 0;
    if (rng_open(&r) && mbedtls_pk_parse_public_key(&pk, VALVE_PUBLIC_KEY, sizeof(VALVE_PUBLIC_KEY)) == 0) {
        mbedtls_rsa_context *rsa = mbedtls_pk_rsa(pk);
        const size_t length = mbedtls_rsa_get_len(rsa);
        mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);
        if (length <= out_cap &&
            mbedtls_rsa_rsaes_oaep_encrypt(rsa, mbedtls_ctr_drbg_random, &r.drbg, MBEDTLS_RSA_PUBLIC, NULL, 0,
                                           size, in, out) == 0)
            result = length;
    }
    rng_close(&r);
    mbedtls_pk_free(&pk);
    return result;
}

uint32_t steam_crc32c(const void *data, size_t size)
{
    static uint32_t table[256];
    static bool ready;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (unsigned k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
