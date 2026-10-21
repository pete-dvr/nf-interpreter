// Host tests compile the exact parser/CRC used in ProductFirmwareUpdate.cpp.
// Compile with C++11, -Wall -Wextra -Werror, and -Itargets/ESP32/_include.
// /tmp/product_ota_parser_tests
#include "ProductOtaPackage.h"
#include <assert.h>
#include <stdio.h>

using namespace ProductOtaPackage;

static void Put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
    {
        p[i] = (uint8_t)(value >> (i * 8));
    }
}

static void BaseHeader(uint8_t *bytes, bool signedPackage)
{
    memset(bytes, 0, HeaderLength);
    memcpy(bytes, "PRDOTA01", 8);
    bytes[8] = 1;
    bytes[10] = 128;
    Put32(bytes + 12, signedPackage ? 1 : 0);
    Put32(bytes + 16, 128 + 32 + 64 + (signedPackage ? 64 : 0));
    Put32(bytes + 20, 32);
    Put32(bytes + 24, 64);
    Put32(bytes + 28, 2);
    memcpy(bytes + 32, TargetId, sizeof(TargetId));
    memcpy(bytes + 48, "test-release", 12);
}

int main()
{
    const uint8_t vector[] = "123456789";
    assert((UpdateCrc(UINT32_MAX, vector, 9) ^ UINT32_MAX) == 0xCBF43926);
    uint32_t crc = UpdateCrc(UINT32_MAX, vector, 4);
    assert((UpdateCrc(crc, vector + 4, 5) ^ UINT32_MAX) == 0xCBF43926);
    assert((UpdateCrc(UINT32_MAX, vector, 0) ^ UINT32_MAX) == 0);
    uint8_t bytes[HeaderLength];
    Header parsed;
    BaseHeader(bytes, false);
    assert(ParseHeader(bytes, sizeof(bytes), 224, false, parsed));
    assert(!ParseHeader(bytes, sizeof(bytes), 224, true, parsed));
    BaseHeader(bytes, true);
    assert(ParseHeader(bytes, sizeof(bytes), 288, true, parsed));
    assert(parsed.runtimeLength == 32 && parsed.managedLength == 64 && parsed.flags == 1);
    assert(!ParseHeader(bytes, 127, 288, true, parsed));
    assert(!ParseHeader(bytes, sizeof(bytes), 287, true, parsed));
    assert(!ParseHeader(bytes, sizeof(bytes), 289, true, parsed));
    assert(!ParseHeader(bytes, sizeof(bytes), UINT64_C(0x100000120), true, parsed));
    for (unsigned offset = 0; offset < 64; ++offset)
    {
        if (offset >= 48)
        {
            continue;
        }
        uint8_t changed[HeaderLength];
        memcpy(changed, bytes, sizeof(changed));
        changed[offset] ^= 0x80;
        assert(!ParseHeader(changed, sizeof(changed), 288, true, parsed));
    }
    Put32(bytes + 20, UINT32_MAX);
    Put32(bytes + 24, 1);
    Put32(bytes + 16, 192); // The apparent total after a 32-bit wrap.
    assert(!ParseHeader(bytes, sizeof(bytes), 192, true, parsed));
    BaseHeader(bytes, true);
    Put32(bytes + 20, 0);
    Put32(bytes + 16, 256);
    assert(!ParseHeader(bytes, sizeof(bytes), 256, true, parsed));
    BaseHeader(bytes, true);
    Put32(bytes + 24, 0);
    Put32(bytes + 16, 224);
    assert(!ParseHeader(bytes, sizeof(bytes), 224, true, parsed));
    BaseHeader(bytes, true);
    Put32(bytes + 20, RuntimeCapacity);
    Put32(bytes + 24, ManagedCapacity);
    Put32(bytes + 16, HeaderLength + RuntimeCapacity + ManagedCapacity + 64);
    assert(ParseHeader(bytes, sizeof(bytes), 0x4800C0, true, parsed));
    Put32(bytes + 20, RuntimeCapacity + 1);
    Put32(bytes + 16, 0x4800C1);
    assert(!ParseHeader(bytes, sizeof(bytes), 0x4800C1, true, parsed));
    BaseHeader(bytes, true);
    Put32(bytes + 24, ManagedCapacity + 1);
    Put32(bytes + 16, HeaderLength + 32 + ManagedCapacity + 1 + 64);
    assert(!ParseHeader(bytes, sizeof(bytes), HeaderLength + 32 + ManagedCapacity + 1 + 64, true, parsed));
    BaseHeader(bytes, true);
    bytes[48] = 0;
    assert(!ParseHeader(bytes, sizeof(bytes), 288, true, parsed));
    BaseHeader(bytes, true);
    bytes[49] = 0;
    assert(!ParseHeader(bytes, sizeof(bytes), 288, true, parsed));
    BaseHeader(bytes, true);
    bytes[48] = ' ';
    assert(!ParseHeader(bytes, sizeof(bytes), 288, true, parsed));
    BaseHeader(bytes, true);
    memset(bytes + 48, 'r', 16);
    assert(ParseHeader(bytes, sizeof(bytes), 288, true, parsed));
    puts("Product OTA native parser and CRC tests passed");
    return 0;
}
