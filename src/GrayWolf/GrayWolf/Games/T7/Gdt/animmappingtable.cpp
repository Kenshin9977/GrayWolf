#include "stdafx.h"

// BO3 animmappingtable (76) -> its .ai_am file (the table read by AnimMappingTable.h). Written in
// type.jsonc's "files" folder: a "#" line, then each alias and its animations, each followed by ",".
// No GDT entry: the Mod Tools' share/raw/animtables holds these files.

#include "AnimMappingTable.h"

namespace T7Gdt
{
    AnimMap ReadAnimMappingTable(const Gdt::Memory& M, int64_t Pos)
    {
        AnimMap Out;
        const int64_t Maps = M.Follow(Pos + 8);
        const int32_t Count = M.At<int32_t>(Pos + 0x10);
        for (int32_t I = 0; Maps >= 0 && I < Count && I < 1 << 20; I++)
        {
            const int64_t Map = Maps + 0x18 * (int64_t)I;
            std::vector<std::string> Anims;
            const int64_t List = M.Follow(Map + 8);
            const int32_t AnimCount = M.At<int32_t>(Map + 0x10);
            for (int32_t A = 0; List >= 0 && A < AnimCount && A < 65536; A++) Anims.push_back(M.ScriptString(M.At<uint32_t>(List + 4 * (int64_t)A)));
            Out.emplace_back(M.ScriptString(M.At<uint32_t>(Map)), std::move(Anims));
        }
        return Out;
    }
}

namespace
{
    bool Convert(const Gdt::Type& T, const Gdt::Memory& M, int64_t Pos, const std::string& Name, std::vector<Gdt::Block>&, Gdt::Stats& S)
    {
        std::string Out = "#\r\n";
        for (const auto& [Alias, Anims] : T7Gdt::ReadAnimMappingTable(M, Pos))
        {
            Out += Alias + ",";
            for (const auto& A : Anims) Out += A + ",";
            Out += "\r\n";
        }
        S.Files.emplace_back(T.Settings().value("files", std::string()) + Name, std::move(Out));
        return true;
    }
}

GDT_CONVERTER(t7_animmappingtable, Convert);
