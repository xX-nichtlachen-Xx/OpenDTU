// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace IntelHex {

enum class RowResult : uint8_t { Data, Eof, Skip, Error };

inline RowResult decodeRow(const char* ascii, size_t asciiLen, uint8_t* out, size_t& outLen)
{
    outLen = 0;

    while (asciiLen > 0 && (ascii[asciiLen - 1] == '\r' || ascii[asciiLen - 1] == '\n' || ascii[asciiLen - 1] == ' ' || ascii[asciiLen - 1] == '\t')) {
        --asciiLen;
    }
    while (asciiLen > 0 && (ascii[0] == ' ' || ascii[0] == '\t')) {
        ++ascii;
        --asciiLen;
    }

    if (asciiLen == 0 || ascii[0] != ':') {
        return RowResult::Skip;
    }

    if (asciiLen < 3 || (asciiLen - 1) % 2 != 0) {
        return RowResult::Error;
    }

    const size_t totalBytes = (asciiLen - 1) / 2;
    for (size_t i = 0; i < totalBytes; ++i) {
        const char hi = ascii[1 + 2 * i];
        const char lo = ascii[2 + 2 * i];
        const auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return -1;
        };
        const int niHi = nibble(hi);
        const int niLo = nibble(lo);
        if (niHi < 0 || niLo < 0) {
            return RowResult::Error;
        }
        out[i] = static_cast<uint8_t>((niHi << 4) | niLo);
    }

    if (totalBytes == 0) {
        return RowResult::Skip;
    }
    outLen = totalBytes - 1;

    const uint8_t recordType = totalBytes >= 4 ? out[3] : 0xFF;
    return recordType == 0x01 ? RowResult::Eof : RowResult::Data;
}

} // namespace IntelHex
