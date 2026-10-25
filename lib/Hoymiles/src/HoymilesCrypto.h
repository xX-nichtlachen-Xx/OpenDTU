// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

/*
HoymilesCrypto — payload encryption for HM 1.5.x (and later) inverters.
*/
class HoymilesCrypto {
public:
    static void deriveKeyIv(
        const uint8_t seed[16],
        const uint8_t sn4[4],
        const uint32_t unixTime,
        uint8_t keyOut[16],
        uint8_t ivOut[16]);

    static void deriveConfigKeyIv(
        const uint8_t seed[16],
        const uint8_t sn4[4],
        uint8_t keyOut[16],
        uint8_t ivOut[16]);

    static void encryptBlock(const uint8_t key[16], const uint8_t iv[16],
        const uint8_t* in, uint8_t* out, const size_t len);
    static void decryptBlock(const uint8_t key[16], const uint8_t iv[16],
        const uint8_t* in, uint8_t* out, const size_t len);

private:
    static void sha256_triple(const uint8_t* msg, const size_t len, uint8_t out16[16]);
};
