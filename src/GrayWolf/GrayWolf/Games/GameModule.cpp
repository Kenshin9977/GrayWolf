#include "stdafx.h"

#include "GameModule.h"

#include "../FastFile.h"

#include <filesystem>

namespace
{
    std::vector<GameModule>& AddedModules()
    {
        static std::vector<GameModule> Rows;
        return Rows;
    }
}

GameModuleRegistration::GameModuleRegistration(std::initializer_list<GameModule> Rows)
{
    AddedModules().insert(AddedModules().end(), Rows.begin(), Rows.end());
}

const std::vector<GameModule>& RegisteredGameModules()
{
    // Version 0 means the magic alone identifies the title; titles sharing a magic differ by version.
    // The titles read through OAT or the legacy walkers; the others register from Games/<game>/.
    static const std::vector<GameModule> Modules = [] {
        std::vector<GameModule> Rows = {
            { "IW3", "Call of Duty 4",       "iw3sp.exe",           "IWffu100", 5,    GameSupportStage::Assets },
            { "T4",  "World at War",         "codwaw.exe",          "TAffu100", 0,    GameSupportStage::Assets },
            { "T4",  "World at War",         "codwaw.exe",          "IWffu100", 387,  GameSupportStage::Assets },   // the PC zones
            { "T5",  "Black Ops",            "BlackOps.exe",        "TAff0100", 0,    GameSupportStage::Assets },
            { "T5",  "Black Ops",            "BlackOps.exe",        "IWffu100", 473,  GameSupportStage::Assets },   // the PC zones
            { "T6",  "Black Ops 2",          "t6zm.exe",            "TAff0100", 147,  GameSupportStage::Assets },
            { "T6",  "Black Ops 2",          "t6zm.exe",            "TAffu100", 147,  GameSupportStage::Assets },   // unsigned (OAT's Linker)
            { "IW4", "Modern Warfare 2",     "iw4sp.exe",           "IWffu100", 276,  GameSupportStage::Assets },
            { "IW4", "Modern Warfare 2",     "iw4sp.exe",           "IWff0100", 276,  GameSupportStage::Assets },   // signed zones
            { "IW5", "Modern Warfare 3",     "iw5sp.exe",           "IWffu100", 1,    GameSupportStage::Assets },
            { "IW5", "Modern Warfare 3",     "iw5sp.exe",           "IWff0100", 1,    GameSupportStage::Assets },   // signed zones
        };
        Rows.insert(Rows.end(), AddedModules().begin(), AddedModules().end());
        return Rows;
    }();
    return Modules;
}

namespace
{
    std::string LowerAscii(std::string S)
    {
        for (auto& C : S)
            if (C >= 'A' && C <= 'Z') C = (char)(C - 'A' + 'a');
        return S;
    }

    // Among rows sharing a magic and version, the one FastFilePath belongs to
    const GameModule* Disambiguate(const std::vector<const GameModule*>& Rows, const std::string& FastFilePath)
    {
        if (!FastFilePath.empty())
        {
            // The nearest folder above the file that names a row's install folder
            std::error_code Ec;
            auto Dir = std::filesystem::absolute(std::filesystem::path(FastFilePath), Ec).parent_path();
            for (; !Dir.empty(); Dir = Dir.parent_path())
            {
                const auto Name = LowerAscii(Dir.filename().string());
                for (const auto* Row : Rows)
                    if (Row->InstallFolder != nullptr && Name == LowerAscii(Row->InstallFolder))
                        return Row;
                if (Dir == Dir.parent_path())
                    break;
            }
            for (const auto* Row : Rows)
                if (Row->ClaimsHeader != nullptr && Row->ClaimsHeader(FastFilePath))
                    return Row;
        }
        for (const auto* Row : Rows)
            if (Row->InstallFolder == nullptr && Row->ClaimsHeader == nullptr)
                return Row;
        return nullptr;
    }
}

const GameModule* FindGameModuleForFile(const std::string& FastFilePath)
{
    const auto Info = FastFile::Identify(FastFilePath);
    return FindGameModule(Info.Magic, Info.Version, FastFilePath);
}

const GameModule* FindGameModule(const std::string& Magic, const uint32_t Version, const std::string& FastFilePath)
{
    const GameModule* MagicOnly = nullptr;
    std::vector<const GameModule*> Exact;

    for (const auto& Module : RegisteredGameModules())
    {
        if (Magic != Module.FastFileMagic)
            continue;

        if (Module.FastFileVersion == Version)
        {
            Exact.push_back(&Module);
            continue;
        }

        // Remember a magic-only match, but keep looking: a title that names its version is the
        // more specific answer and should win over one that does not.
        if (Module.FastFileVersion == 0 && MagicOnly == nullptr)
            MagicOnly = &Module;
    }

    if (Exact.size() == 1)
        return Exact.front();
    if (!Exact.empty())
        return Disambiguate(Exact, FastFilePath);
    return MagicOnly;
}
