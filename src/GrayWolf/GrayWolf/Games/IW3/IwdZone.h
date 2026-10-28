#pragma once

#include "ByteView.h"

// The IW3 family on the disk template (CoD4, World at War): a ZonekitZone served to the game's live
// readers, whose images are .iwi files of the install's .iwd archives (IWDCache, by name) or, for
// the code images, the pixels the zone carries (GfxImageLoadDef), and whose sounds are files of
// the archives' sound folder, named by the aliases, or loaded sounds the zone holds. What differs
// per title is a few layouts, given by the subclass (Games/IW3/DiskGame.cpp, Games/T4/DiskGame.cpp).

#include "Games/IW3/Iw3Install.h"
#include "Games/IW/Iw32Level.h"
#include "Games/IW/IWSoundAliases.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Iw3Family
{
    // the level: Games/IW/Iw32Level (MapLayout, Map(), Material() given by the title)
    class IwdZone : public Iw32LevelZone
    {
    public:
        IwdZone(const std::string& Path, std::string InstallDir);
        ~IwdZone() override;

        // the index keeps what Defines gives: its images and loaded sounds too
        std::string IndexKey() const override { return ZonekitZone::IndexKey() + "_images_sounds1"; }
        void DefinesAt(std::vector<std::pair<DiskGame::AssetId, int64_t>>& Out) const override;

    protected:
        // its XFile's size: where the asset list (and its script strings' count) starts
        virtual int64_t XFileSize() const = 0;
        // the live image handler (GameModernWarfare / GameWorldAtWar::LoadXImage)
        virtual LoadXImageHandler ImageHandler() const = 0;
        // snd_alias_t: its size and where its SoundFile pointer is; SoundFile: type u8 at 0 (1:
        // loaded, its LoadedSound pointer at +4), else a streamed file's folder and name pointers
        struct SoundLayout
        {
            int64_t AliasSize, AliasFile, StreamDir, StreamName;
            int64_t LoadedSize;   // in a LoadedSound: its data size (0: the samples are elsewhere)
        };
        virtual SoundLayout Sounds() const = 0;
        // the loaded sounds of the zone, written (CoD4: PCM behind a header; WaW: whole files)
        virtual void ExportLoadedSounds(const DiskGame::Context& C, DiskGame::Totals& T) = 0;
        // sound_aliases: the snd_alias_t's fields (IWSoundAliases), and the file ExportLoadedSounds
        // writes for a loaded sound of that name, relative to the sounds folder
        virtual IWSoundAliases::Layout AliasLayout() const = 0;
        virtual std::string LoadedSoundFile(const std::string& Name) const = 0;
        bool DerefAssetPointers() const override { return true; }
        VirtualGameSession::Handlers Handlers() override;
        void Served(bool On) override;
        void Fixups() override;
        std::string AssetName(DiskGame::Kind K, int64_t Pos) const override;

        const std::string& InstallDir() const { return Install; }

    private:
        std::string Install;
        std::shared_ptr<IWDCache> Iwd;

        void Serve(bool On);
        std::vector<std::string> ZoneStrings(ByteView Z) const;
        void ExportImages(const DiskGame::Context& C, DiskGame::Totals& T);
        std::unique_ptr<XImageDDS> Decode(const std::string& ImageName, int64_t Pos);
        // an image of the .iwd archives, as Decode reads it first: its .iwi's (Sizes::IwiWeight);
        // one in the zone weighs its GfxImageLoadDef there
        void Weigh(DiskGame::Kind K, int64_t Pos, DiskGame::Weight& W) const override;
        std::unique_ptr<XImageDDS> LoadDef(int64_t Pos);
        void ExportSounds(const DiskGame::Context& C, DiskGame::Totals& T);
        void ExportSoundFiles(const DiskGame::Context& C, DiskGame::Totals& T);
        void ExportSoundAliases(const DiskGame::Context& C, DiskGame::Totals& T);
        // the file the sound kind wrote for the SoundFile at File (a zone offset), "" for none
        std::string AliasFile(int64_t File) const;
    };
}
