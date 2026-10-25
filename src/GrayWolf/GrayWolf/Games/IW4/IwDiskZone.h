#pragma once

// Modern Warfare 2 and 3 (the 64-bit rebuild) on the disk template: a ZonekitZone with the iw4 or
// iw5 profile, served to the 64-bit readers (Games/IW4/Iw64Readers.h). What is theirs: images
// either in the zone (a GfxImageLoadDef) or in the .iwd archives of the install's main folder,
// loaded sounds as PCM in the zone.

#include "Games/IW/IWMaterialBooks.h"
#include "Games/IW/IWZone.h"
#include "Games/IW4/Iw64Readers.h"
#include "Games/OAT/OatWeapons.h"

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class IWDCache;

namespace Iw64
{
    struct GameInfo
    {
        const char* Id;         // --game
        const char* Title;
        const char* Engine;     // GameModule engine
        const char* Profile;    // zonekit profile
        const char* Live;       // its live game's engine (Games/<ENGINE>/LiveGame.cpp)
        const Layout* Readers;
        Scripts::Vm ScriptVm{};   // its scriptfiles' VM (MW3); MW2's scripts are source rawfiles
        const DiskGame::IDiskGame::LevelNaming* Levels = nullptr;   // its IDiskGame::LevelNames
    };

    class IwDiskZone final : public IWZone
    {
    public:
        IwDiskZone(const std::string& Path, const GameInfo& G);
        ~IwDiskZone() override;
        // the string tables, and the .arena rawfiles (the multiplayer map list)
        void Documents(std::vector<std::pair<std::string, std::string>>& Out) const override;

    protected:
        const char* Profile() const override { return Game.Profile; }
        const ICoDGame& LiveGame() const override { return LiveGames::Get(Game.Live); }
        VirtualGameSession::Handlers Handlers() override { return { SlotString, DiskImage }; }
        // an asset pointer's back reference names the slot holding it (resolve_deref, 0xf3f60)
        bool DerefAssetPointers() const override { return true; }
        void Fixups() override;
        // the readers find the material a model's ",name" header stands for among this zone's
        // (Iw64::MaterialDefinedAt)
        void Served(bool On) override;
        // the models and animations, and the images this zone holds whole (pixels or .iwd name)
        void Defines(std::vector<DiskGame::AssetId>& Out) const override;
        std::string IndexKey() const override { return ZonekitZone::IndexKey() + "|iw64 images 1"; }
        // the level of a map zone (Games/IW4/IwMap.cpp): world, placements, entity models, lights, sky
        bool ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models) override;

    private:
        static std::unique_ptr<XImageDDS> DiskImage(const XImage_t& Image);

        std::unique_ptr<XImageDDS> Decode(int64_t Pos, uint64_t& Key);
        void ExportImages(const DiskGame::Context& C, DiskGame::Totals& T);
        void ExportSounds(const DiskGame::Context& C, DiskGame::Totals& T);
        void ExportStreamedSounds(const DiskGame::Context& C, DiskGame::Totals& T);
        void ExportSoundAliases(const DiskGame::Context& C, DiskGame::Totals& T);
        void ExportRawfiles(const DiskGame::Context& C, DiskGame::Totals& T);
        void ExportStringTables(const DiskGame::Context& C, DiskGame::Totals& T);
        // the n bytes a relocated pointer names, inflated when Packed (zlib) to Size
        bool Payload(uint64_t Pointer, uint32_t N, bool Packed, uint32_t Size, std::vector<uint8_t>& Out) const;
        // a rawfile's content, inflated
        bool RawfileBytes(int64_t Pos, std::vector<uint8_t>& Out) const;
        // the install's .iwd archives, indexed once per install (shared by every zone of a run)
        IWDCache* Packages();

        const GameInfo& Game;
        OatWeapons::Source Weapons;   // the weapon kinds, through OpenAssetTools
        // the materials this zone defines whole, by lower-case name: their offsets in the image
        std::unordered_map<std::string, int64_t> Materials;
        bool MaterialsRead = false;
        void ReadMaterials();
        // The material headers this zone does not define: the zones the install index says define
        // them, booked (Games/IW/IWMaterialBooks.h). Once per zone, before its models or its map
        // are read (the index may be built then, this zone's session parked meanwhile).
        void BookMaterialsElsewhere();
        bool BooksTried = false;
    };

    // The IDiskGame of one title: its zones are the unsigned fast files (IWffu100) of the input
    class IwDiskGame final : public DiskGame::IDiskGame
    {
    public:
        explicit IwDiskGame(const GameInfo& G) : Game(G) {}
        const char* Id() const override { return Game.Id; }
        const char* Title() const override { return Game.Title; }
        const char* Engine() const override { return Game.Engine; }
        std::unique_ptr<DiskGame::IDiskZone> Unopened() const override { return std::make_unique<IwDiskZone>(std::string(), Game); }
        const LevelNaming* LevelNames() const override { return Game.Levels; }
        bool Zones(const std::string& Input, const DiskGame::ExportOptions& Options, std::vector<std::string>& Out,
                   std::string& Error) const override;
        bool ListZones(const std::string& Install, const DiskGame::ExportOptions& Options, std::vector<std::string>& Names,
                       std::string& Error) const override;
        std::unique_ptr<DiskGame::IDiskZone> Open(const std::string& Path, const DiskGame::ExportOptions& Options,
                                                  std::string& Error) const override;

    private:
        const GameInfo& Game;
    };
}
