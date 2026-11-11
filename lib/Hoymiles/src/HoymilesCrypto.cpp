// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 OpenDTU contributors
 */
#include "HoymilesCrypto.h"
#include <cstring>
#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

static inline void sha256_once(const uint8_t* in, const size_t len, uint8_t out[32])
{
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
    mbedtls_sha256(in, len, out, 0 /* = SHA-256, not SHA-224 */);
#else
    mbedtls_sha256_ret(in, len, out, 0);
#endif
}

void HoymilesCrypto::sha256_triple(const uint8_t* msg, const size_t len, uint8_t out16[16])
{
    uint8_t digest[32];
    sha256_once(msg, len, digest);
    sha256_once(digest, sizeof(digest), digest);
    sha256_once(digest, sizeof(digest), digest);
    memcpy(out16, digest, 16);
}

void HoymilesCrypto::deriveKeyIv(
    const uint8_t seed[16],
    const uint8_t sn4[4],
    const uint32_t unixTime,
    uint8_t keyOut[16],
    uint8_t ivOut[16])
{
    const uint16_t t1 = static_cast<uint16_t>((unixTime / 0x12C0000UL) & 0xFFFF);
    const uint16_t t2 = static_cast<uint16_t>((unixTime / 300UL) & 0xFFFF);
    const uint8_t timeBytes[4] = {
        static_cast<uint8_t>(t1 >> 8), static_cast<uint8_t>(t1 & 0xFF),
        static_cast<uint8_t>(t2 >> 8), static_cast<uint8_t>(t2 & 0xFF)
    };

    uint8_t keyPre[20];
    memcpy(&keyPre[0], sn4, 4);
    memcpy(&keyPre[4], &seed[0], 12);
    memcpy(&keyPre[16], timeBytes, 4);
    sha256_triple(keyPre, sizeof(keyPre), keyOut);

    uint8_t ivPre[16];
    memcpy(&ivPre[0], sn4, 4);
    memcpy(&ivPre[4], &seed[8], 4);
    memcpy(&ivPre[8], timeBytes, 4);
    memcpy(&ivPre[12], &seed[12], 4);
    sha256_triple(ivPre, sizeof(ivPre), ivOut);
}

void HoymilesCrypto::deriveConfigKeyIv(
    const uint8_t seed[16],
    const uint8_t sn4[4],
    uint8_t keyOut[16],
    uint8_t ivOut[16])
{
    uint8_t keyPre[16];
    memcpy(&keyPre[0], sn4, 4);
    memcpy(&keyPre[4], &seed[0], 12);
    sha256_triple(keyPre, sizeof(keyPre), keyOut);

    uint8_t ivPre[12];
    memcpy(&ivPre[0], sn4, 4);
    memcpy(&ivPre[4], &seed[8], 8);
    sha256_triple(ivPre, sizeof(ivPre), ivOut);
}

void HoymilesCrypto::encryptBlock(const uint8_t key[16], const uint8_t iv[16],
    const uint8_t* in, uint8_t* out, const size_t len)
{
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    uint8_t ivCopy[16];
    memcpy(ivCopy, iv, 16);
    mbedtls_aes_setkey_enc(&ctx, key, 128);
    mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, len & ~static_cast<size_t>(0x0F), ivCopy, in, out);
    mbedtls_aes_free(&ctx);
}

void HoymilesCrypto::decryptBlock(const uint8_t key[16], const uint8_t iv[16],
    const uint8_t* in, uint8_t* out, const size_t len)
{
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    uint8_t ivCopy[16];
    memcpy(ivCopy, iv, 16);
    mbedtls_aes_setkey_dec(&ctx, key, 128);
    mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, len & ~static_cast<size_t>(0x0F), ivCopy, in, out);
    mbedtls_aes_free(&ctx);
}
