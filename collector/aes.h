// aes.h - AES-128 in counter mode, the cipher Meshtastic channels use. Only the
// encrypt direction exists: CTR decrypts by encrypting the counter.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct { uint8_t rk[176]; } Aes128;

void aes128_init(Aes128 *a, const uint8_t key[16]);
void aes128_encrypt_block(const Aes128 *a, const uint8_t in[16], uint8_t out[16]);

// XORs data with the keystream from a 16-byte initial counter (big-endian
// increment of the whole block, as Meshtastic's firmware does).
void aes128_ctr(const Aes128 *a, const uint8_t iv[16], uint8_t *data, size_t len);
