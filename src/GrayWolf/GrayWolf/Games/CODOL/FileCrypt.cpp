#include "stdafx.h"

#include "Games/CODOL/FileCrypt.h"
#include "ZipInflate.h"

#include <bcrypt.h>
#include <miniz/miniz.h>

#include <cstring>

#pragma comment(lib, "bcrypt.lib")

namespace
{
    // The key the game unseals at start (0x66d0d0: the 0x100 bytes at 0xda9708 through the fast
    // files' RSA key, OAEP SHA-256), as dtzxporter's WraithXOL (MIT) also has it
    const uint8_t FileKey[24] = { 0x15, 0x9a, 0x03, 0x25, 0xe0, 0x75, 0x2e, 0x80, 0xc6, 0xc0, 0x94, 0x2a,
                                  0x50, 0x5c, 0x1c, 0x68, 0x8c, 0x17, 0xef, 0x53, 0x99, 0xf8, 0x68, 0x3c };
    constexpr size_t BlockSize = 0x8000;

    uint32_t NameNonce(const std::string& Base)
    {
        // CRC-32 (reflected, 0xEDB88320) whose register starts at ~length instead of ~0
        uint32_t Crc = ~(uint32_t)Base.size();
        for (const char Ch : Base)
        {
            Crc ^= (uint8_t)Ch;
            for (int K = 0; K < 8; K++) Crc = (Crc >> 1) ^ (0xEDB88320u & (0u - (Crc & 1)));
        }
        return ~Crc;
    }

    struct Aes
    {
        BCRYPT_ALG_HANDLE Alg = nullptr;
        BCRYPT_KEY_HANDLE Key = nullptr;
        Aes()
        {
            if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&Alg, BCRYPT_AES_ALGORITHM, nullptr, 0)) &&
                BCRYPT_SUCCESS(BCryptSetProperty(Alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB,
                                                 sizeof(BCRYPT_CHAIN_MODE_ECB), 0)))
                BCryptGenerateSymmetricKey(Alg, &Key, nullptr, 0, (PUCHAR)FileKey, sizeof(FileKey), 0);
        }
        ~Aes()
        {
            if (Key) BCryptDestroyKey(Key);
            if (Alg) BCryptCloseAlgorithmProvider(Alg, 0);
        }
    };
}

bool Codol::UnwrapFile(const std::string& Name, std::vector<uint8_t>& Data)
{
    if (Data.size() < 6) return false;
    std::string Base = Name;
    if (const auto Slash = Base.find_last_of("/\\"); Slash != std::string::npos) Base = Base.substr(Slash + 1);
    uint32_t Unpacked = 0;
    std::memcpy(&Unpacked, Data.data() + Data.size() - 4, 4);
    const size_t Packed = Data.size() - 4;
    if (Unpacked == 0 || Unpacked > (1u << 30)) return false;

    Aes A;
    if (A.Key == nullptr) return false;
    std::vector<uint8_t> Plain(Data.begin(), Data.begin() + Packed);
    const uint32_t Nonce = NameNonce(Base);
    std::vector<uint8_t> Stream;
    for (size_t Done = 0; Done < Packed; Done += BlockSize)
    {
        const size_t N = std::min(BlockSize, Packed - Done);
        uint8_t Counter[16];
        const uint32_t Fields[4] = { Nonce, Unpacked, (uint32_t)Done, (uint32_t)N };
        std::memcpy(Counter, Fields, 16);
        Stream.resize((N + 15) / 16 * 16);
        for (size_t At = 0; At < Stream.size(); At += 16)
        {
            std::memcpy(Stream.data() + At, Counter, 16);
            for (int B = 15; B >= 0 && ++Counter[B] == 0; B--) {}
        }
        ULONG Produced = 0;
        if (!BCRYPT_SUCCESS(BCryptEncrypt(A.Key, Stream.data(), (ULONG)Stream.size(), nullptr, nullptr, 0, Stream.data(),
                                          (ULONG)Stream.size(), &Produced, 0)))
            return false;
        for (size_t I = 0; I < N; I++) Plain[Done + I] ^= Stream[I];
    }
    if (Plain.empty() || Plain[0] != 0x78) return false;
    std::vector<uint8_t> Out(Unpacked);
    size_t Got = 0;
    if (!ZipInflate::Zlib(Plain.data(), Plain.size(), Out.data(), Out.size(), Got) || Got != Unpacked) return false;
    Data = std::move(Out);
    return true;
}
