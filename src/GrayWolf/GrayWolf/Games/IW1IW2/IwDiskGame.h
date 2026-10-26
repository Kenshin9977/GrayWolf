#pragma once

// CoD1 (IW1) and CoD2 (IW2) on the disk template: a zone is one .pk3/.iwd archive, its assets the
// files whose copy the game reads is in it (IwInstall.h); a model reads its parts, surfaces,
// materials and textures wherever the install keeps them. Kinds: model (with its textures, written
// by the image kind's writer), anim, image, sound, map, script, localize, weapons and weaponstats
// (the weapon files, Games/Common/WeaponFiles.h), fx (the .efx files, EfxEffects.h). One
// implementation; each game gives its row.

#include "DiskGame.h"

namespace IW12
{
    struct GameRow
    {
        const char* Id;          // --game and the output folder
        const char* Title;
        const char* Engine;
        const char* Extension;   // ".pk3", ".iwd"
        // Models name their textures directly (CoD1: skins/<name>.dds) rather than through material
        // files (CoD2: materials/<name> -> images/<name>.iwi).
        bool TexturesByName;
        // How its archives are named: by the maps they hold (DiskGame::LevelNames)
        const DiskGame::IDiskGame::LevelNaming* Levels = nullptr;
    };

    class ArchiveGame final : public DiskGame::IDiskGame
    {
    public:
        explicit ArchiveGame(const GameRow& Row) : Row(Row) {}
        const char* Id() const override { return Row.Id; }
        const char* Title() const override { return Row.Title; }
        const char* Engine() const override { return Row.Engine; }
        // language: whose localized archives replace the base's files (english: none)
        const std::vector<DiskGame::Setting>& Settings() const override;
        std::unique_ptr<DiskGame::IDiskZone> Unopened() const override;
        const LevelNaming* LevelNames() const override { return Row.Levels; }
        bool Zones(const std::string& Input, const DiskGame::ExportOptions& Options, std::vector<std::string>& Out,
                   std::string& Error) const override;
        bool ListZones(const std::string& Install, const DiskGame::ExportOptions& Options, std::vector<std::string>& Names,
                       std::string& Error) const override;
        std::unique_ptr<DiskGame::IDiskZone> Open(const std::string& Zone, const DiskGame::ExportOptions& Options,
                                                  std::string& Error) const override;
        // the anims' weapons, AI and player types, from the install's files (Games/IwClassic/IwClassicAnimOwners.h)
        void AnimOwners(const DiskGame::ExportOptions& Options, const std::vector<AnimZone>& Zones, ::AnimOwners::Book& Out) const override;

    private:
        GameRow Row;
    };
}
