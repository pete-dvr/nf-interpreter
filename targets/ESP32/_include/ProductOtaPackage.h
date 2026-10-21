//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

#ifndef PRODUCT_OTA_PACKAGE_H
#define PRODUCT_OTA_PACKAGE_H

// Deliberately independent of ESP-IDF, so the device parser and CRC can be tested
// on the host with the very same implementation.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace ProductOtaPackage
{
static const uint32_t HeaderLength = 128;
static const uint32_t RuntimeCapacity = 0x1A0000;
static const uint32_t ManagedCapacity = 0x2E0000;
static const uint32_t LayoutId = 2;
static const uint8_t TargetId[16] = {'P', 'R', '-', 'S', '3', '-', 'N', '1', '6', 'R', '8', '-', '0', '1', 0, 0};

struct Header
{
    uint32_t flags;
    uint32_t totalLength;
    uint32_t runtimeLength;
    uint32_t managedLength;
    uint8_t releaseId[16];
    uint8_t runtimeSha256[32];
    uint8_t managedSha256[32];
};

inline uint16_t Read16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t Read32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline bool ValidIdentifier(const uint8_t *id)
{
    bool padding = false;
    if (id[0] == 0)
    {
        return false;
    }
    for (size_t i = 0; i < 16; ++i)
    {
        if (id[i] == 0)
        {
            padding = true;
        }
        else if (padding || id[i] < 0x21 || id[i] > 0x7e)
        {
            return false;
        }
    }
    return true;
}

inline bool ParseHeader(const uint8_t *bytes, size_t available, uint64_t actualLength, bool requireSignature, Header &out)
{
    if (available != HeaderLength || memcmp(bytes, "PRDOTA01", 8) != 0 || Read16(bytes + 8) != 1 ||
        Read16(bytes + 10) != HeaderLength || Read32(bytes + 28) != LayoutId ||
        memcmp(bytes + 32, TargetId, sizeof(TargetId)) != 0 || !ValidIdentifier(bytes + 48))
    {
        return false;
    }
    const uint32_t flags = Read32(bytes + 12);
    const uint32_t runtimeLength = Read32(bytes + 20);
    const uint32_t managedLength = Read32(bytes + 24);
    const uint64_t total = static_cast<uint64_t>(HeaderLength) + runtimeLength + managedLength + ((flags & 1) ? 64 : 0);
    if ((flags & ~1U) != 0 || (requireSignature && !(flags & 1)) || runtimeLength == 0 || managedLength == 0 ||
        runtimeLength > RuntimeCapacity || managedLength > ManagedCapacity || total > UINT32_MAX ||
        total != actualLength || total != Read32(bytes + 16))
    {
        return false;
    }
    out.flags = flags;
    out.totalLength = static_cast<uint32_t>(total);
    out.runtimeLength = runtimeLength;
    out.managedLength = managedLength;
    memcpy(out.releaseId, bytes + 48, sizeof(out.releaseId));
    memcpy(out.runtimeSha256, bytes + 64, sizeof(out.runtimeSha256));
    memcpy(out.managedSha256, bytes + 96, sizeof(out.managedSha256));
    return true;
}

// CRC-32/ISO-HDLC: initial 0xffffffff, reflected 0xedb88320, final xor
// 0xffffffff. Call UpdateCrc repeatedly, then xor once after the entire file.
inline uint32_t UpdateCrc(uint32_t crc, const uint8_t *bytes, size_t length)
{
    for (size_t i = 0; i < length; ++i)
    {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
        {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return crc;
}
} // namespace ProductOtaPackage
#endif
