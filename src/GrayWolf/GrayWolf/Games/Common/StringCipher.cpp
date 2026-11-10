#include "stdafx.h"

#include "Games/Common/StringCipher.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

struct StringCipher::Table
{
    struct Pair
    {
        uint8_t R = 0, K = 0;
    };
    std::array<std::vector<int>, 0x40> Select;   // by id - 0x80: the cipher per b1, -1 none
    std::vector<std::vector<Pair>> Ciphers;      // per position
    bool Loaded = false;
    std::string Error;
};

namespace
{
    using Table = StringCipher::Table;

    int Hex(char C) { return C >= '0' && C <= '9' ? C - '0' : C >= 'a' && C <= 'f' ? C - 'a' + 10 : C >= 'A' && C <= 'F' ? C - 'A' + 10 : -1; }

    bool Read(const std::string& Path, Table& T)
    {
        std::ifstream In(Path);
        if (!In)
        {
            T.Error = "cannot open " + Path;
            return false;
        }
        std::string Line;
        while (std::getline(In, Line))
        {
            std::istringstream L(Line);
            std::string Word;
            if (!(L >> Word) || Word[0] == '#') continue;
            if (Word == "select")
            {
                std::string Id;
                L >> Id;
                const unsigned long V = std::stoul(Id, nullptr, 16);
                if (V < 0x80 || V > 0xBF) continue;
                auto& Row = T.Select[V - 0x80];
                int N = 0;
                while (L >> N) Row.push_back(N);
            }
            else if (Word == "cipher")
            {
                size_t N = 0;
                std::string Pairs;
                L >> N >> Pairs;
                if (N != T.Ciphers.size() || Pairs.size() % 3 != 0)
                {
                    T.Error = Path + ": cipher " + std::to_string(N) + " malformed";
                    return false;
                }
                std::vector<Table::Pair> C(Pairs.size() / 3);
                for (size_t I = 0; I < C.size(); I++)
                {
                    const int R = Hex(Pairs[I * 3]), K1 = Hex(Pairs[I * 3 + 1]), K2 = Hex(Pairs[I * 3 + 2]);
                    if (R < 0 || R > 7 || K1 < 0 || K2 < 0)
                    {
                        T.Error = Path + ": cipher " + std::to_string(N) + " malformed";
                        return false;
                    }
                    C[I] = { (uint8_t)R, (uint8_t)(K1 * 16 + K2) };
                }
                T.Ciphers.push_back(std::move(C));
            }
        }
        for (const auto& Row : T.Select)
            for (const int N : Row)
                if (N >= (int)T.Ciphers.size())
                {
                    T.Error = Path + ": a select names cipher " + std::to_string(N) + ", the table has " + std::to_string(T.Ciphers.size());
                    return false;
                }
        return true;
    }
}

const StringCipher::Table* StringCipher::Load(const std::string& Dir, std::string& Error)
{
    static std::mutex Lock;
    static std::map<std::string, std::unique_ptr<Table>> Tables;
    std::lock_guard<std::mutex> Guard(Lock);
    auto& T = Tables[Dir];
    if (T == nullptr)
    {
        T = std::make_unique<Table>();
        // GRAYWOLF_NO_STRING_CIPHER=1: as on a machine without the tables (they are not shipped),
        // for the regress's checks of what a user gets
        if (GetEnvironmentVariableA("GRAYWOLF_NO_STRING_CIPHER", nullptr, 0) != 0) T->Error = "GRAYWOLF_NO_STRING_CIPHER";
        else T->Loaded = Read((std::filesystem::path(Dir) / "strings.cipher.txt").string(), *T);
    }
    Error = T->Error;
    return T->Loaded ? T.get() : nullptr;
}

bool StringCipher::Decrypt(const Table* Cipher, const std::string& Raw, std::string& Text)
{
    const auto Id = Raw.empty() ? 0 : (uint8_t)Raw[0];
    if ((Id & 0xC0) != 0x80)
    {
        Text = Raw;
        return true;
    }
    if (Cipher == nullptr || Raw.size() < 3) return false;
    const auto& Row = Cipher->Select[Id - 0x80];
    const auto B1 = (uint8_t)Raw[1];
    if (B1 >= Row.size() || Row[B1] < 0) return false;
    const auto& C = Cipher->Ciphers[(size_t)Row[B1]];
    if (Raw.size() - 3 > C.size()) return false;
    std::string Out(Raw.size() - 3, '\0');
    for (size_t I = 0; I < Out.size(); I++)
    {
        const auto Byte = (uint8_t)Raw[I + 3];
        const uint8_t V = Byte != C[I].K ? (uint8_t)(Byte ^ C[I].K) : Byte;
        const uint8_t R = C[I].R & 7;
        Out[I] = (char)(R ? (uint8_t)((V << R) | (V >> (8 - R))) : V);
    }
    Text = std::move(Out);
    return true;
}
