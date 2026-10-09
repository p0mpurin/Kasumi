#pragma once

/* The cryptography Steam Remote Play uses, on mbedTLS 2.28.
 *
 * Symmetric encryption is Steam's CCrypto: AES-CBC with PKCS#7 padding under
 * a 16 byte (session) or 32 byte (pairing secret) key. "steam_sym_*" puts a
 * random IV in front, itself encrypted with AES-ECB; control messages use
 * the frame form instead, whose IV is HMAC-MD5(key, sequence || message). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool steam_random(void *out, size_t size);

/* IV-prefixed AES-CBC. out needs size + 32 bytes; returns the length, 0 on failure. */
size_t steam_sym_encrypt(const uint8_t *key, size_t key_size, const void *in, size_t size,
                         uint8_t *out, size_t out_cap);
/* Returns the plaintext length, or -1. out needs `size` bytes. */
int steam_sym_decrypt(const uint8_t *key, size_t key_size, const uint8_t *in, size_t size,
                      uint8_t *out, size_t out_cap);
/* AES-CBC with a given IV and PKCS#7 padding removed; -1 on failure. */
int steam_cbc_decrypt(const uint8_t *key, size_t key_size, const uint8_t iv[16], const uint8_t *in,
                      size_t size, uint8_t *out, size_t out_cap);

/* Control-channel frames: iv(16) || AES-CBC(seq_le64 || message). Encrypt
 * returns the length (out needs size + 40 bytes); decrypt checks the HMAC
 * and gives the sequence and the message (in place in `out`). */
size_t steam_frame_encrypt(const uint8_t *key, size_t key_size, uint64_t sequence, const void *in,
                           size_t size, uint8_t *out, size_t out_cap);
int steam_frame_decrypt(const uint8_t *key, size_t key_size, const uint8_t *in, size_t size,
                        uint8_t *out, size_t out_cap, uint64_t *sequence, const uint8_t **message);

bool steam_hmac_sha256(const uint8_t *key, size_t key_size, const void *in, size_t size, uint8_t out[32]);
bool steam_sha256(const void *in, size_t size, uint8_t out[32]);

/* The pairing key exchange (X25519, RFC 7748): a key pair, and Steam's
 * shared secret SHA256(X25519(private, peer)). */
bool steam_x25519_keypair(uint8_t private_key[32], uint8_t public_key[32]);
bool steam_key_exchange(const uint8_t private_key[32], const uint8_t peer_public[32], uint8_t secret[32]);

/* RSA-OAEP (SHA-1) under Valve's public universe key, for the key escrow
 * ticket in a pairing request. Returns the length (256), 0 on failure. */
size_t steam_rsa_encrypt(const void *in, size_t size, uint8_t *out, size_t out_cap);

/* CRC-32C (Castagnoli), the session packets' checksum. */
uint32_t steam_crc32c(const void *data, size_t size);
