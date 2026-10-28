#include "stdafx.h"

#include "SettingsManager.h"

#include "FileSystems.h"
#include "BinaryReader.h"
#include "Hashing.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <mutex>
#include <thread>

std::unordered_map<std::string, std::string> SettingsManager::SettingsCache;
std::unordered_map<std::string, std::string> SettingsManager::RunOverrides;
std::string SettingsManager::SettingsFileName = "";

namespace
{
    // the settings are read from the export's threads while the GUI's sets them (recursive: a
    // setting set saves the file)
    std::recursive_mutex SettingsLock;
}

void SettingsManager::LoadSettings(const std::string& SettingsName, const std::map<std::string, std::string>& Defaults)
{
    LoadSettings(SettingsName, "", Defaults);
}

void SettingsManager::LoadSettings(const std::string& SettingsName, const std::string& LegacyName,
                                   const std::map<std::string, std::string>& Defaults)
{
    std::error_code Ec;
    SettingsCache.clear();

    auto CurrentPath = FileSystems::GetApplicationPath();
    auto ConfigPath = (std::filesystem::path(CurrentPath) / (SettingsName + ".json")).string();

    SettingsFileName = ConfigPath;

    // The program's former name: its settings are read once, then saved under the new one
    bool Migrated = false;
    if (!LegacyName.empty() && !std::filesystem::exists(ConfigPath, Ec))
    {
        const auto LegacyPath = (std::filesystem::path(CurrentPath) / (LegacyName + ".json")).string();
        if (std::filesystem::is_regular_file(LegacyPath, Ec))
        {
            ConfigPath = LegacyPath;
            Migrated = true;
        }
    }

    bool Readable = true;
    if (std::filesystem::is_regular_file(ConfigPath, Ec))
    {
        // Another GrayWolf (a parallel headless run) may be saving it right now: an unreadable
        // file is read again, and if it stays so it is left alone rather than overwritten with
        // the defaults
        Readable = false;
        for (int Attempt = 0; Attempt < 20 && !Readable; Attempt++)
        {
            std::ifstream Reader(ConfigPath, std::ios::binary);
            if (Reader)
            {
                std::stringstream JsonPayload;
                JsonPayload << Reader.rdbuf();
                try
                {
                    SettingsCache = nlohmann::json::parse(JsonPayload.str(), nullptr, true, true);
                    Readable = true;
                }
                catch (...)
                {
                    SettingsCache.clear();
                }
            }
            if (!Readable)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    // Fallback to old style binary settings, but remove it and save new json.
    else
    {
        ConfigPath = (std::filesystem::path(CurrentPath) / (SettingsName + ".wcfg")).string();

        if (std::filesystem::is_regular_file(ConfigPath, Ec))
        {
            BinaryReader Reader;
            Reader.Open(ConfigPath);
            if (Reader.Read<uint32_t>() == 0x47464357)
            {
                auto Count = Reader.Read<uint32_t>();
                for (uint32_t i = 0; i < Count; i++)
                {
                    auto Key = Reader.ReadNullTerminatedString();
                    auto Value = Reader.ReadNullTerminatedString();
                    SettingsCache.insert(std::make_pair(Key, Value));
                }
            }
        }

        std::filesystem::remove(ConfigPath, Ec);
    }

    bool Added = false;
    for (auto& Keys : Defaults)
    {
        if (SettingsCache.find(Keys.first) == SettingsCache.end())
        {
            SettingsCache.insert(std::make_pair(Keys.first, Keys.second));
            Added = true;
        }
    }

    // Resave: only what was readable and gained a default (a load writes nothing otherwise)
    if (!Readable)
    {
        SettingsFileName.clear();       // this run keeps its settings in memory only
        return;
    }
    if (Added || Migrated)
        SaveSettings();
}

void SettingsManager::SaveSettings()
{
    std::lock_guard<std::recursive_mutex> G(SettingsLock);
    if (SettingsFileName.empty())
    {
        return;
    }
    // Written beside it then renamed over it: a reader (another GrayWolf) never sees a half
    // written file
    const auto Temp = SettingsFileName + "." + std::to_string(GetCurrentProcessId()) + ".tmp";
    {
        std::ofstream Writer(Temp);
        if (!Writer)
        {
            return;
        }
        Writer << nlohmann::json(SettingsCache).dump(1);
    }
    std::error_code Ec;
    std::filesystem::rename(Temp, SettingsFileName, Ec);
    if (Ec)
    {
        std::filesystem::remove(Temp, Ec);
    }
}

std::string SettingsManager::GetSetting(const std::string& Key, const std::string& Default)
{
    std::lock_guard<std::recursive_mutex> G(SettingsLock);
    if (const auto Run = RunOverrides.find(Key); Run != RunOverrides.end())
        return Run->second;
    if (SettingsCache.find(Key) != SettingsCache.end())
    {
        return ModifyValue(Key, SettingsCache.at(Key));
    }
    SettingsCache.insert(std::make_pair(Key, UnModifyValue(Key, Default)));
    return Default;
}

void SettingsManager::SetSetting(const std::string& Key, const std::string& Value)
{
    std::lock_guard<std::recursive_mutex> G(SettingsLock);
    RunOverrides.erase(Key);
    auto NewValue = UnModifyValue(Key, Value);

    if (SettingsCache.find(Key) != SettingsCache.end())
    {
        SettingsCache.at(Key) = NewValue;
    }
    else
    {
        SettingsCache.insert(std::make_pair(Key, NewValue));
    }

    SaveSettings();
}

void SettingsManager::SetRunSetting(const std::string& Key, const std::string& Value)
{
    std::lock_guard<std::recursive_mutex> G(SettingsLock);
    RunOverrides[Key] = Value;
}

void SettingsManager::ClearRunSetting(const std::string& Key)
{
    std::lock_guard<std::recursive_mutex> G(SettingsLock);
    RunOverrides.erase(Key);
}

std::string SettingsManager::ModifyValue(const std::string& Key, const std::string& Value)
{
    auto KeyHash = Hashing::HashXXHashString(Key);
    if (KeyHash == 0x90007daf3980ef1f)    // Password
    {
        std::string Result = Value;
        const std::string KeyBuffer = "L9VuBReup51wLQ";
        auto SizeCache = (uint32_t)KeyBuffer.size();
        for (uint32_t i = 0; i < (uint32_t)Result.size(); i++)
        {
            Result[i] ^= KeyBuffer[i % SizeCache];
        }
        return Result;
    }

    return Value;
}

std::string SettingsManager::UnModifyValue(const std::string& Key, const std::string& Value)
{
    auto KeyHash = Hashing::HashXXHashString(Key);
    if (KeyHash == 0x90007daf3980ef1f)    // Password
    {
        std::string Result = Value;
        const std::string KeyBuffer = "L9VuBReup51wLQ";
        auto SizeCache = (uint32_t)KeyBuffer.size();
        for (uint32_t i = 0; i < (uint32_t)Result.size(); i++)
        {
            Result[i] ^= KeyBuffer[i % SizeCache];
        }
        return Result;
    }

    return Value;
}