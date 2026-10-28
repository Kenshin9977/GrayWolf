#pragma once

#include <string>
#include <map>
#include <unordered_map>

class SettingsManager
{
private:
    static std::unordered_map<std::string, std::string> SettingsCache;
    // Values for this run only (SetRunSetting): read first, never saved
    static std::unordered_map<std::string, std::string> RunOverrides;
    static std::string SettingsFileName;

    static std::string ModifyValue(const std::string& Key, const std::string& Value);
    static std::string UnModifyValue(const std::string& Key, const std::string& Value);

public:

    static void LoadSettings(const std::string& SettingsName, const std::map<std::string, std::string>& Defaults);
    // Same, but when <SettingsName>.json is missing and <LegacyName>.json exists beside the exe,
    // reads the old file (left in place) and writes <SettingsName>.json from then on
    static void LoadSettings(const std::string& SettingsName, const std::string& LegacyName,
                             const std::map<std::string, std::string>& Defaults);
    // Saves the settings file to the disk, must have loaded first
    static void SaveSettings();

    static std::string GetSetting(const std::string& Key, const std::string& Default = "");
    static void SetSetting(const std::string& Key, const std::string& Value);
    // Sets a setting for this run only (a command line flag): read before the saved value, never
    // written to the settings file; a later SetSetting of the key replaces it
    static void SetRunSetting(const std::string& Key, const std::string& Value);
    // The saved value read again (a run setting set for one piece of work)
    static void ClearRunSetting(const std::string& Key);
};