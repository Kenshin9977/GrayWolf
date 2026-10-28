#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

class Hashing
{
public:

    static std::string HashCRC32String(const std::string& Value, uint32_t Initial = 0);

    static uint64_t HashXXHashString(const std::string& Value);

    // FNV-1a (64-bit) of Value, continued from H: what the tables kept on disk are keyed by
    static uint64_t Fnv1a(const std::string& Value, uint64_t H = 0xcbf29ce484222325ull)
    {
        for (const unsigned char C : Value) H = (H ^ C) * 0x100000001b3ull;
        return H;
    }

    // V as 16 lower-case hex digits
    static std::string Hex16(uint64_t V)
    {
        char B[20];
        snprintf(B, sizeof(B), "%016llx", (unsigned long long)V);
        return B;
    }
};