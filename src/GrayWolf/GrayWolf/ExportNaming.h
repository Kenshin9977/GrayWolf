#pragma once

// Small helpers every offline exporter needs: case folding, file-safe names, the comma-separated
// substring name filter, a bounded read from a zone, numbers as text, and debug switches read from the environment.

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace ExportNaming
{
    inline std::string Lower(std::string S)
    {
        for (auto& C : S) C = (char)std::tolower((unsigned char)C);
        return S;
    }

    // Replaces the characters Windows refuses in a file name.
    inline std::string SafeFileName(std::string S)
    {
        for (auto& C : S)
            if (C == '\\' || C == '/' || C == ':' || C == '*' || C == '?' || C == '"' || C == '<' ||
                C == '>' || C == '|')
                C = '_';
        return S;
    }

    // FNV-1a (64-bit) of S, continued from H: what the kept indexes (zones, packages) are keyed by.
    inline uint64_t Fnv1a(const std::string& S, uint64_t H = 0xcbf29ce484222325ull)
    {
        for (unsigned char C : S) H = (H ^ C) * 0x100000001b3ull;
        return H;
    }

    // V as 16 lower-case hex digits.
    inline std::string Hex16(uint64_t V)
    {
        char B[20];
        snprintf(B, sizeof(B), "%016llx", (unsigned long long)V);
        return B;
    }

    // 1630 as "1,630"
    inline std::string Thousands(size_t N)
    {
        std::string S = std::to_string(N);
        for (int At = (int)S.size() - 3; At > 0; At -= 3) S.insert((size_t)At, ",");
        return S;
    }

    // a float as its shortest text, read back as the double that prints the same (0.1f -> 0.1)
    inline double Shortest(float F)
    {
        const std::string S = std::format("{}", F);
        try
        {
            return std::stod(S);
        }
        catch (...)
        {
            return (double)F;
        }
    }

    // The T at Off in a zone (or any buffer), T{} when it does not fit.
    template <class T>
    T At(const std::vector<uint8_t>& Buf, int64_t Off)
    {
        T V{};
        if (Off >= 0 && Off + (int64_t)sizeof(T) <= (int64_t)Buf.size()) std::memcpy(&V, Buf.data() + Off, sizeof(T));
        return V;
    }

    // Character base name: strip a trailing _LOD<n>, then one trailing part token (_body/_head/
    // _hat/...), so a body + head + gear that share a base assemble into one preview/model. The
    // one rule the CLI's batch preview and the GUI's Assets-tab model assembly both use.
    inline std::string CharacterBaseName(std::string N)
    {
        const auto Lp = N.rfind("_LOD");
        if (Lp != std::string::npos && Lp + 4 < N.size() && N.find_first_not_of("0123456789", Lp + 4) == std::string::npos)
            N.erase(Lp);
        static const char* Parts[] = { "_body","_head","_hat","_helmet","_gear","_arms","_legs",
            "_hands","_viewhands","_torso","_fullbody","_face","_mask","_backpack","_hair","_teeth","_eyes" };
        for (const char* P : Parts)
        {
            const std::string Ps(P);
            if (N.size() > Ps.size() && N.compare(N.size() - Ps.size(), Ps.size(), Ps) == 0) { N.erase(N.size() - Ps.size()); break; }
        }
        return N;
    }

    // "a,b" (or "a, b") keeps names containing a or b, case-insensitively; an empty filter keeps
    // everything.
    class NameFilter
    {
    public:
        NameFilter() = default;
        explicit NameFilter(const std::string& Csv)
        {
            const std::string All = Lower(Csv);
            for (size_t At = 0; At <= All.size() && !All.empty();)
            {
                size_t Comma = All.find(',', At);
                if (Comma == std::string::npos) Comma = All.size();
                const size_t First = All.find_first_not_of(' ', At);
                const size_t Last = All.find_last_not_of(' ', Comma - 1);
                if (First < Comma && Last != std::string::npos && Last >= First) Parts.push_back(All.substr(First, Last - First + 1));
                At = Comma + 1;
            }
        }

        bool Empty() const { return Parts.empty(); }

        bool Matches(const std::string& Name) const
        {
            if (Parts.empty()) return true;
            const std::string L = Lower(Name);
            for (const auto& P : Parts)
                if (L.find(P) != std::string::npos) return true;
            return false;
        }

    private:
        std::vector<std::string> Parts;
    };
}
