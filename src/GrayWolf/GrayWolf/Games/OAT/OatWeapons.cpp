#include "stdafx.h"

#include "OatWeapons.h"

#include "ExportWriters.h"
#include "Games/Common/WeaponFiles.h"
#include "OatInProcess.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <format>

namespace fs = std::filesystem;

namespace
{
    // a file's asset name: its path in OAT's tree without the folder (weapons/ak47_mp: ak47_mp)
    std::string AssetOf(const std::string& Path)
    {
        const size_t Slash = Path.find('/');
        return Slash == std::string::npos ? Path : Path.substr(Slash + 1);
    }

    // MW3's attachment, OAT's attachment/<name>.json ("_game": "iw5"), as an attachment file:
    // displayName, attachmentType, every other value as iw5.<field> (a block's as iw5.<block>.<field>,
    // the block itself iw5.<block> 1), for WeaponFiles' MW3 attachment rules. Empty when not one.
    std::string Iw5AttachmentFile(const std::string& Text)
    {
        const auto J = nlohmann::json::parse(Text, nullptr, false);
        if (!J.is_object() || J.value("_game", "") != "iw5" || J.value("_type", "") != "attachment") return {};
        std::string Out = "ATTACHMENTFILE\\iw5.format\\1";
        const auto Put = [&](const std::string& K, const nlohmann::json& V) {
            std::string S;
            if (V.is_boolean()) S = V.get<bool>() ? "1" : "0";
            else if (V.is_number_integer()) S = std::to_string(V.get<int64_t>());
            else if (V.is_number()) S = std::format("{}", V.get<double>());
            else if (V.is_string()) S = V.get<std::string>();
            else return;
            for (char& C : S)
                if (C == '\\' || C == '\n' || C == '\r') C = ' ';
            Out += "\\" + K + "\\" + S;
        };
        for (const auto& [K, V] : J.items())
        {
            if (K.starts_with("_")) continue;
            if (K == "displayName") Put("displayName", V);
            else if (K == "type") Put("attachmentType", V);
            else if (V.is_object())
            {
                Put("iw5." + K, true);
                for (const auto& [F, X] : V.items()) Put("iw5." + K + "." + F, X);
            }
            else Put("iw5." + K, V);
        }
        return Out;
    }
}

namespace OatWeapons
{
    Source::Source(std::string FastFile) : Path(std::move(FastFile)) {}
    Source::Source(OatZoneSession* L) : Lent(L), Tried(true) {}
    Source::~Source() = default;

    OatZoneSession* Source::Session(std::string& Error)
    {
        if (Lent != nullptr) return Lent;
        if (!Tried)
        {
            Tried = true;
            Owned = OatZoneSession::Open(Path, OpenError);
            if (!Owned && OpenError.empty()) OpenError = "OpenAssetTools does not load the zone";
        }
        if (!Owned) Error = "OpenAssetTools: " + OpenError;
        return Owned.get();
    }

    bool Source::Files(const std::vector<std::pair<std::string, std::string>>*& Out, size_t& Assets, std::string& Error)
    {
        OatZoneSession* S = Session(Error);
        if (S == nullptr) return false;
        if (!Listed)
        {
            Listed = true;
            if (!S->WeaponFiles(Cache, CachedAssets)) Error = "OAT's weapon dump failed";
        }
        Out = &Cache;
        Assets = CachedAssets;
        return true;
    }

    bool Source::Strings(std::vector<std::pair<std::string, std::string>>& Out, unsigned& CodePage, std::string& Error)
    {
        OatZoneSession* S = Session(Error);
        if (S == nullptr) return false;
        S->Strings(Out);
        CodePage = ExportWriters::LanguageCodePage(S->FastFile());
        return true;
    }

    bool Source::Tables(const std::vector<std::string>& Names, std::map<std::string, std::string>& Out, std::string& Error)
    {
        OatZoneSession* S = Session(Error);
        if (S == nullptr) return false;
        std::vector<std::pair<std::string, std::string>> All;
        S->StringTables(All);
        for (auto& [Path, Text] : All)
        {
            std::string Key = Path;
            for (char& C : Key) C = C == '\\' ? '/' : (char)std::tolower((unsigned char)C);
            if (std::find(Names.begin(), Names.end(), Key) != Names.end()) Out[Key] = std::move(Text);
        }
        return true;
    }

    void ExportWeapons(Source& S, const std::string& Zone, const DiskGame::Context& C, DiskGame::Totals& T)
    {
        using DiskGame::Kind;
        auto& Kt = T.Kinds[Kind::Weapons];
        const std::vector<std::pair<std::string, std::string>>* Files = nullptr;
        size_t Assets = 0;
        std::string Error;
        if (!S.Files(Files, Assets, Error))
        {
            T.Fail(Zone, Kind::Weapons, "", Error);
            return;
        }
        if (!Error.empty()) T.Fail(Zone, Kind::Weapons, "", Error);
        Kt.Seen += Assets;
        if (Files->empty()) return;
        const fs::path Root(C.OutRoot), Dir = C.KindDir(Kind::Weapons);
        for (size_t N = 0; N < Files->size(); N++)
        {
            const auto& [Path, Text] = (*Files)[N];
            const std::string Name = fs::path(AssetOf(Path)).filename().string();
            if (!C.Advance(N, Files->size(), Name)) break;
            if (!C.Wants(Kind::Weapons, Name)) continue;
            // weapons/<name> as OAT's tree has it (the kind's folder), the rest beside it under weapons/
            const fs::path To = Path.rfind("weapons/", 0) == 0 ? Root / Path : Dir / Path;
            if (ExportWriters::WriteBytes(To, (const uint8_t*)Text.data(), Text.size())) Kt.Written++;
            else T.Fail(Zone, Kind::Weapons, Path, "write failed");
        }
    }


    void ExportWeaponStats(Source& S, const std::string& Zone, const DiskGame::Context& C, DiskGame::Totals& T, const char* Title)
    {
        using DiskGame::Kind;
        const std::vector<std::pair<std::string, std::string>>* Files = nullptr;
        size_t Assets = 0;
        std::string Error;
        if (!S.Files(Files, Assets, Error))
        {
            T.Fail(Zone, Kind::WeaponStats, "", Error);
            return;
        }
        if (!Error.empty()) T.Fail(Zone, Kind::WeaponStats, "", Error);
        std::vector<WeaponFiles::Weapon> Weapons;
        // the weapon files, and Black Ops II's attachment files (attachment/<name>) for the attachment table
        for (const auto& [Path, Text] : *Files)
            if (Path.rfind("weapons/", 0) == 0 || (Path.rfind("attachment/", 0) == 0 && WeaponFiles::IsAttachmentFile(Text)))
                Weapons.push_back({ AssetOf(Path), Text, {} });
            // MW3's attachments (OAT writes them as JSON)
            else if (Path.rfind("attachment/", 0) == 0 && Path.ends_with(".json"))
                if (std::string File = Iw5AttachmentFile(Text); !File.empty())
                    Weapons.push_back({ fs::path(AssetOf(Path)).stem().string(), std::move(File), {} });
        std::vector<std::pair<std::string, std::string>> Strings;
        unsigned CodePage = 0;
        S.Strings(Strings, CodePage, Error);
        for (auto& [K, V] : Strings) V = ExportWriters::Utf8(V, CodePage);
        // where each game keeps its weapons' names (found by exporting the language zones, 2026-10-21)
        const std::string Title_ = Title;
        const char* Where =
            Title_ == "Call of Duty: World at War"
                ? "The display names are the localized strings of another zone (World at War: the WEAPON_* strings are in\n"
                  "zone/<language>/localized_common_mp for multiplayer, zone/<language>/common for the campaign and zombies;\n"
                  "localized_code_post_gfx_mp and code_post_gfx hold none): export it into the same --out. A weapon a patch\n"
                  "zone holds (patch_mp) is read from it, the game loading it after the zone it patches; zones lists every\n"
                  "zone holding the weapon.\n"
            : Title_ == "Call of Duty: Black Ops" || Title_ == "Call of Duty: Black Ops II"
                ? "The display names are the localized strings of another zone (Black Ops, Black Ops II: the WEAPON_* strings\n"
                  "are in zone/<language>/<lang>_code_post_gfx_mp for multiplayer; zombies and the campaign: Black Ops II's\n"
                  "<lang>_code_post_gfx_zm, Black Ops' <lang>_code_post_gfx; <lang>_common_mp, <lang>_patch_mp add none):\n"
                  "export it into the same --out. A weapon a patch zone holds (patch_mp) is read\n"
                  "from it, the game loading it after the zone it patches; zones lists every zone holding the weapon.\n"
            : Title_ == "Call of Duty 4: Modern Warfare" || Title_ == "Call of Duty: Modern Warfare 2" || Title_ == "Call of Duty: Modern Warfare 3"
                ? "The display names are the localized strings of another zone (CoD4, MW2, MW3: the WEAPON_* strings are in\n"
                  "zone/<language>/localized_code_post_gfx_mp for multiplayer and zone/<language>/common for the campaign\n"
                  "and Special Ops; localized_common_mp, localized_ui_mp, code_post_gfx_mp and the patch zones add none;\n"
                  "found by exporting them, 2026-10-22): export it into the same --out. Weapons naming a key no zone holds\n"
                  "(MW3: WEAPON_MK12SPR, WEAPON_SAM, WEAPON_TROPHY... in no zone read) or none (killstreak and script\n"
                  "weapons) stay unnamed. A weapon a patch zone holds (patch_mp) is read from it, the game loading it\n"
                  "after the zone it patches; zones lists every zone holding the weapon.\n"
                : "The display names are the localized strings of another zone (CoD4, World at War: localized_code_post_gfx_mp;\n"
                  "Black Ops, Black Ops II: <lang>_code_post_gfx_mp...): export it into the same --out. A weapon a patch zone\n"
                  "holds (patch_mp) is read from it, the game loading it after the zone it patches; zones lists every zone\n"
                  "holding the weapon.\n";
        const char* const Names = Where;
        static const char* const From =
            "The zone is loaded by OpenAssetTools, whose weapon dumper writes each weapon as the mod tools' weapon\n"
            "file (weapons/<name>, WEAPONFILE\\<field>\\<value>...). Its field names and their places in the\n"
            "WeaponDef are the game's own weapon field table (the one its weapon files are parsed with, OAT's\n"
            "WeaponFields.h), so each column below is one named field, its unit the field's type: times are\n"
            "milliseconds in the asset (OAT writes them in seconds, read back here), distances inches.\n";
        // MW3: the class builder's table of the attachment pairs it refuses (code_post_gfx_mp), for the
        // combinations (WeaponFiles)
        std::map<std::string, std::string> Tables;
        if (Title_ == "Call of Duty: Modern Warfare 3") S.Tables({ "mp/attachmentcombos.csv" }, Tables, Error);
        WeaponFiles::ExportStats(Weapons, Strings, Zone, C, T, { Title, Names, From }, Tables);
    }
}
