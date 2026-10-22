#include "stdafx.h"

#include "Games/IW/IwLanguageNames.h"

#include "CoDAssets.h"
#include "WraithNameIndex.h"

#include <algorithm>
#include <cctype>

namespace
{
    // IwctxReaders::AssetHash, from a state: FNV-1a 64 of the lower-case text, '\' as '/'
    uint64_t Continue(uint64_t H, std::string_view Text)
    {
        for (const char C : Text)
        {
            char L = (char)std::tolower((unsigned char)C);
            if (L == '\\') L = '/';
            H = (H ^ (uint8_t)L) * 0x100000001B3ull;
        }
        return H;
    }
    constexpr uint64_t Basis = 0x47F5817A5EF961BAull, Mask = 0x7FFFFFFFFFFFFFFFull;
}

IwLanguageNames::IwLanguageNames(std::vector<std::string_view> Languages) : Suffixes(std::move(Languages))
{
    // "all": a sound every language shares (the index's names end in it the most)
    Suffixes.push_back("all");
}

// every index name whose last part is one of the suffixes, once per stem (the names the index
// already holds under another language share it)
void IwLanguageNames::Build()
{
    size_t Longest = 0;
    uint64_t Lengths = 0;    // bit n: a language n characters long
    for (const auto S : Suffixes)
    {
        Longest = std::max(Longest, S.size());
        Lengths |= 1ull << S.size();
    }
    const std::unordered_set<std::string_view> Known(Suffixes.begin(), Suffixes.end());
    std::unordered_set<uint64_t> Seen;
    char Last[64];
    for (const auto& [Key, N] : CoDAssets::AssetNameCache.NameDatabase)
    {
        // the last part, looked for only as far back as the longest language
        size_t Dot = N.size();
        for (size_t K = 0; K <= Longest && K < N.size(); K++)
            if (N[N.size() - 1 - K] == '.')
            {
                Dot = N.size() - 1 - K;
                break;
            }
        if (Dot == N.size() || Dot == 0) continue;
        const size_t Length = N.size() - Dot - 1;
        if (!(Lengths >> Length & 1)) continue;
        for (size_t K = 0; K < Length; K++) Last[K] = (char)std::tolower((unsigned char)N[Dot + 1 + K]);
        if (!Known.count(std::string_view(Last, Length))) continue;
        const uint64_t H = Continue(Basis, std::string_view(N).substr(0, Dot + 1));
        if (!Seen.insert(H).second) continue;
        Stems.push_back({ H, (uint32_t)Pool.size(), (uint32_t)(Dot + 1) });
        Pool.append(N, 0, Dot + 1);
    }
}

std::unordered_map<uint64_t, std::string> IwLanguageNames::Name(const std::unordered_set<uint64_t>& Unknown)
{
    std::unordered_map<uint64_t, std::string> Found;
    if (Unknown.empty()) return Found;
    std::call_once(Built, [this] { Build(); });
    for (const auto& S : Stems)
        for (const auto Suffix : Suffixes)
        {
            uint64_t H = S.Hash;    // (the suffixes are lower case already)
            for (const char C : Suffix) H = (H ^ (uint8_t)C) * 0x100000001B3ull;
            H &= Mask;
            if (Unknown.count(H)) Found.try_emplace(H, Pool.substr(S.At, S.Length) + std::string(Suffix));
        }
    return Found;
}

size_t IwLanguageNames::Rename(const std::vector<uint64_t>& Hashes, std::vector<IwctxReaders::SoundEntry>& Found)
{
    std::unordered_set<uint64_t> Unknown;
    for (size_t K = 0; K < Found.size(); K++)
        if (Found[K].Full.rfind("xsound_", 0) == 0) Unknown.insert(Hashes[K] & Mask);
    const auto Named = Name(Unknown);
    size_t Count = 0;
    for (size_t K = 0; K < Found.size() && !Named.empty(); K++)
        if (const auto It = Named.find(Hashes[K] & Mask); It != Named.end() && Found[K].Full.rfind("xsound_", 0) == 0)
        {
            Found[K].Full = It->second;
            Count++;
        }
    return Count;
}
