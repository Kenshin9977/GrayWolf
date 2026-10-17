#include "stdafx.h"

#include "ExportWriters.h"

// Black Ops III (T7) on the disk template: a ZonekitZone with the t7 profile, served to the live
// BO3 readers (GameBlackOps3). What is BO3's own: bone
// names from the zone's script strings (Bo3ZoneSlotStrings), pixels and streamed meshes from the
// .xpak packages beside the zone (XPAKCache, Bo3DescribeZoneImage), and sounds from the zone's
// snd/<locale> banks (Bo3ExportZoneSounds, zm_genesis' Salsa20 bank included).

#include "Bo3SlotStrings.h"
#include "Bo3SoundAliases.h"
#include "Bo3ZoneDataTables.h"
#include "Bo3ZoneImages.h"
#include "Bo3ZoneMaterials.h"
#include "Bo3ZoneRawfiles.h"
#include "Bo3ZoneSounds.h"
#include "CoDAssets.h"
#include "DiskGame.h"
#include "Games/Treyarch/TreyarchEffects.h"
#include "GameBlackOps3.h"
#include "Games/Treyarch/TreyarchMaps.h"
#include "MapLive.h"
#include "XPAKCache.h"
#include "XboxPackages.h"
#include "ZonekitZone.h"

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <set>
#include <unordered_map>

namespace fs = std::filesystem;

namespace
{
    // GfxImage: the name pointer at 0xf8 (formats: Bo3ZoneImages.cpp)
    constexpr int64_t ImageNameAt = 0xf8;

    // The packages beside a zone, loaded once per folder for the run: a BO3 zone folder holds
    // dozens of .xpak files and indexing them is the slow part. A Game Pass copy splits the zone
    // folder across its packages (the MP maps' DLC holds no base.xpak): the same folder in the
    // game's other packages too.
    XPAKCache& PackagesFor(const std::string& FastFilePath)
    {
        static std::mutex Lock;
        static std::string Dir;
        static std::unique_ptr<XPAKCache> Cache;
        std::lock_guard<std::mutex> Guard(Lock);
        const std::string Want = fs::path(FastFilePath).parent_path().string();
        if (!Cache || Dir != Want)
        {
            Cache = std::make_unique<XPAKCache>();
            Cache->LoadPackagesIn(Want);
            for (const auto& Other : XboxPackages::SameFolderInGame(Want)) Cache->LoadPackagesIn(Other);
            Dir = Want;
        }
        return *Cache;
    }

    class T7Zone final : public ZonekitZone
    {
    public:
        T7Zone(const std::string& Path, std::string Audio, std::string Acts)
            : ZonekitZone(Path), AudioFormat(std::move(Audio)), ActsExe(std::move(Acts))
        {
            using namespace DiskGame;
            // the live model and anim readers, with the packages beside the zone
            for (Kind K : { Kind::Model, Kind::Anim })
                Exports(K, [this, Base = *ExporterOf(K)](const Context& C, Totals& T) { WithPackages([&] { Base(C, T); }); });
            Exports(Kind::Image, [this](const Context& C, Totals& T) { ExportImages(C, T); });
            Exports(Kind::Sound, [this](const Context& C, Totals& T) { ExportSounds(C, T); });
            Exports(Kind::Rawfile, [this](const Context& C, Totals& T) { ExportRawfiles(C, T); });
            Exports(Kind::Data, [this](const Context& C, Totals& T) { ExportData(C, T); });
            // ScriptParseTree (54) {name, u32 len 8, buffer 0x10}: a GSCOBJ of the T7 VM, decompiled by acts
            Exports(Kind::Script, [this](const Context& C, Totals& T) {
                ExportScripts(C, T, Scripts::Acts, { { "scriptparsetree", [this](int64_t Pos, Scripts::Script& S) {
                                  S.Compiled = true;
                                  const int64_t Payload = Landed(Pos + 0x10);
                                  const uint32_t Size = At<uint32_t>(Zone(), Pos + 8);
                                  if (Payload < 0 || (uint64_t)Payload + Size > Zone().size()) return false;
                                  S.Bytes.assign(Zone().data() + Payload, Zone().data() + Payload + Size);
                                  return true;
                              } } });
            });
            // the placed models read through the live reader, which streams meshes from the packages
            Exports(Kind::Map, [this](const Context& C, Totals& T) { WithPackages([&] { ExportMap(C, T); }); });
            // FxEffectDef (0x90, elements 0x260) as JSON
            Exports(Kind::Fx, [this](const Context& C, Totals& T) {
                const Treyarch::EffectZone Z{ &Image(), [this](uint64_t A) { return Off(A); }, [this](uint64_t A) { return StringAt(A); } };
                ExportEffects(C, T, "fx", [&](int64_t Pos, const std::string& N, std::string& J) {
                    return Treyarch::ReadEffect(Z, Treyarch::EffectLayout::Bo3, Pos, N, J);
                });
            });
            // BO3's own assets as Mod Tools GDT entries (zonekit/profiles/t7/gdt, HydraX's tables)
            ExportsGdt();
        }

    protected:
        const char* Profile() const override { return "t7"; }
        const ICoDGame& LiveGame() const override { return LiveGames::Get("T7"); }
        // pixels the BO3 way: inline after the image, or the largest mip from the packages
        VirtualGameSession::Handlers Handlers() override { return { SlotString, GameBlackOps3::LoadXImage }; }
        // the Ptr readers resolve an asset back reference with resolve_deref (0x1426640)
        bool DerefAssetPointers() const override { return true; }
        void Fixups() override { Slots = Bo3ZoneSlotStrings(Zone()); }
        // A model with no local mesh (LOD0's own XSurfacePtr never resolves in this zone - a
        // shared collision/prop proxy defined once elsewhere) is a reference, not empty content.
        bool IsReference(DiskGame::Kind K, const std::string& Nm, int64_t Pos) const override
        {
            if (K == DiskGame::Kind::Model && !HasLocalMesh(Pos)) return true;
            // a script whose bytecode is not in this zone
            if (K == DiskGame::Kind::Script && (!Marked(Pos + 0x10) || At<uint32_t>(Zone(), Pos + 8) == 0)) return true;
            return ZonekitZone::IsReference(K, Nm, Pos);
        }
        // Localize (22): name at +8, read only when inline (else the entry is a reference); text at
        // +0, inline or a back reference to the same text earlier in the zone (the linker shares a
        // string between entries)
        void LocalizeEntry(int64_t Pos, std::string& Key, std::string& Value) const override
        {
            Key = Text(Pos + 8);
            const int64_t P = Key.empty() ? -1 : Landed(Pos);
            Value = P < 0 ? std::string() : ZoneStr(P);
        }

        // a sound alias by its hash: the zone's sound banks (Sound 10: AliasCount 0x20, the
        // aliases 0x28, 40 bytes each, name 0, hash 8), read once
        std::string AliasName(uint32_t Hash) const override
        {
            if (!AliasesRead)
            {
                AliasesRead = true;
                for (const auto& I : Walk().Instances)
                {
                    if (I.Parent >= 0 || !OwnerIs(I, "sound")) continue;
                    const uint32_t Count = At<uint32_t>(Image(), I.Pos + 0x20);
                    const int64_t List = Off(At<uint64_t>(Image(), I.Pos + 0x28));
                    for (uint32_t K = 0; List >= 0 && K < Count && K < 65536; K++)
                    {
                        const int64_t A = List + 40 * (int64_t)K;
                        const std::string N = StringAt(At<uint64_t>(Image(), A));
                        if (!N.empty()) Aliases.emplace(At<uint32_t>(Image(), A + 8), N);
                    }
                }
            }
            const auto It = Aliases.find(Hash);
            return It == Aliases.end() ? std::string() : It->second;
        }

    private:
        mutable bool AliasesRead = false;
        mutable std::unordered_map<uint32_t, std::string> Aliases;
        // the live readers stream mesh and pixel data through CoDAssets::GamePackageCache
        template <class F>
        void WithPackages(F&& Body)
        {
            XPAKCache& Cache = PackagesFor(FastFilePath());
            auto Saved = std::move(CoDAssets::GamePackageCache);
            CoDAssets::GamePackageCache = std::unique_ptr<CoDPackageCache>(&Cache);
            Body();
            CoDAssets::GamePackageCache.release();
            CoDAssets::GamePackageCache = std::move(Saved);
        }
        void ExportImages(const DiskGame::Context& C, DiskGame::Totals& T);
        // the images with pixels (a package or inline), once per name, that Wants
        void Described(std::vector<Bo3ZoneImage>& Images, size_t& WithoutPixels,
                       const std::function<bool(const std::string&)>& Wants) const;
        void Defines(std::vector<DiskGame::AssetId>& Out) const override;
        void ExportSounds(const DiskGame::Context& C, DiskGame::Totals& T);
        void ExportRawfiles(const DiskGame::Context& C, DiskGame::Totals& T);
        void ExportData(const DiskGame::Context& C, DiskGame::Totals& T);
        bool ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models) override;

        // The raw pointer field at Pos, unrelocated: the exact "== FollowsInline" test that gates a
        // deferred field, as opposed to Landed()'s "does it resolve at all".
        uint64_t Raw(int64_t Pos) const { return At<uint64_t>(Zone(), Pos); }
        bool Marked(int64_t Pos) const { return Raw(Pos) == Rules().Marker; }
        bool MarkedOrResident(int64_t Pos) const
        {
            const uint64_t V = Raw(Pos);
            return V == Rules().Marker || V == Rules().Resident;
        }
        // Where a resolved field's data landed (the relocation Relocate() applied); -1 when nothing
        // resolved it (a cross-zone reference).
        int64_t Landed(int64_t Pos) const
        {
            const uint64_t Addr = At<uint64_t>(Image(), Pos);
            return Addr >= VirtualGameSession::Base && Addr < VirtualGameSession::Base + Image().size()
                ? (int64_t)(Addr - VirtualGameSession::Base) : -1;
        }
        // ExportData's sub-kinds, one per real BO3 data-table shape (Bo3ZoneDataTables.cpp).
        void ExportMaterials(const DiskGame::Context& C, DiskGame::Totals& T);
        void ExportStringTables(const DiskGame::Context& C, DiskGame::Totals& T);
        void ExportStructuredTables(const DiskGame::Context& C, DiskGame::Totals& T);
        void ExportDdl(const DiskGame::Context& C, DiskGame::Totals& T);
        // A Ddl chain node's struct list (stride 0x38: name, then its own member NAMES, count@0xc/
        // list@0x10 stride 0x48) and enum list (stride 0x28: name, member NAME pointers directly,
        // count@0x8/list@0x10 stride 8 - each slot is itself a string pointer, not a struct).
        void ReadDdlStructs(int64_t Arr, uint32_t Count, std::vector<Bo3DdlStructDefCapture>& Out) const;
        void ReadDdlEnums(int64_t Arr, uint32_t Count, std::vector<Bo3DdlEnumDefCapture>& Out) const;

        std::string ZoneStr(int64_t Pos) const
        {
            std::string S;
            const auto& Z = Zone();
            for (int64_t P = Pos; P >= 0 && P < (int64_t)Z.size() && Z[(size_t)P]; P++) S.push_back((char)Z[(size_t)P]);
            return S;
        }
        // A deferred string field, read only when Marked() - the exact-inline gate above.
        std::string Text(int64_t Pos) const
        {
            if (!Marked(Pos)) return std::string();
            const int64_t P = Landed(Pos);
            return P < 0 ? std::string() : ZoneStr(P);
        }
        // Whether this XModel's own LOD0 surfaces resolve inside this zone at all (inline or a
        // same-zone back reference) - live-reader-equivalent, unlike Text()'s exact-inline gate.
        bool HasLocalMesh(int64_t ModelPos) const
        {
            constexpr int64_t Lod0At = 0x88, LodXSurfacePtrAt = 0x68;
            const int64_t LodPos = Landed(ModelPos + Lod0At);
            return LodPos >= 0 && Landed(LodPos + LodXSurfacePtrAt) >= 0;
        }

        std::string AudioFormat, ActsExe;
        size_t DataDone = 0, DataCount = 0;   // ExportData's progress
    };


    void T7Zone::ExportImages(const DiskGame::Context& C, DiskGame::Totals& T)
    {
        auto& Kt = T.Kinds[DiskGame::Kind::Image];
        std::vector<Bo3ZoneImage> Images;
        size_t WithoutPixels = 0;
        Described(Images, WithoutPixels, [&](const std::string& N) { return C.Wants(DiskGame::Kind::Image, N); });
        Counter("images_without_pixels") += WithoutPixels;
        Kt.Seen += Images.size();
        std::string Report;
        const size_t Written = Bo3WriteZoneImages(Zone(), Images, PackagesFor(FastFilePath()), C.KindDir(DiskGame::Kind::Image), "",
                                                  Report, [&](size_t Done, size_t Count) {
                                                      return Done < Images.size() ? C.Advance(Done, Count, Images[Done].Name) : C.Advance(Done, Count);
                                                  });
        Kt.Written += Written;
        if (Written < Images.size() && !C.Cancelled())
        {
            // why, in the writer's words (no_payload, no_dds, no_write)
            std::string Why;
            for (const char* Key : { "no_payload=", "no_dds=", "no_write=" })
            {
                const size_t At = Report.find(Key);
                if (At != std::string::npos) Why += " " + Report.substr(At, Report.find('\n', At) - At);
            }
            T.Fail(Name(), DiskGame::Kind::Image, "", std::to_string(Images.size() - Written) + " not written:" + Why);
        }
    }

    void T7Zone::Described(std::vector<Bo3ZoneImage>& Images, size_t& WithoutPixels,
                           const std::function<bool(const std::string&)>& Wants) const
    {
        XPAKCache& Cache = PackagesFor(FastFilePath());
        // every image instance the walk read (zone-level and inside materials), each once
        std::set<std::string> Names;
        for (const auto& I : Walk().Instances)
        {
            if (!OwnerIs(I, "image")) continue;
            Bo3ZoneImage Image;
            if (At<uint64_t>(Zone(), I.Pos + ImageNameAt) != Rules().Marker || !Bo3DescribeZoneImage(Zone(), (size_t)I.Pos, Cache, Image, fs::path(FastFilePath()).stem().string()))
            {
                WithoutPixels++;
                continue;
            }
            if (Wants(Image.Name) && Names.insert(Image.Name).second) Images.push_back(std::move(Image));
        }
    }

    void T7Zone::Defines(std::vector<DiskGame::AssetId>& Out) const
    {
        ZonekitZone::Defines(Out);
        std::vector<Bo3ZoneImage> Images;
        size_t WithoutPixels = 0;
        Described(Images, WithoutPixels, [](const std::string&) { return true; });
        for (const auto& I : Images) Out.push_back({ DiskGame::Kind::Image, I.Name });
    }

    void T7Zone::ExportSounds(const DiskGame::Context& C, DiskGame::Totals& T)
    {
        auto& Kt = T.Kinds[DiskGame::Kind::Sound];
        std::string Report;
        std::unordered_map<uint32_t, std::string> Files;
        const size_t Written = Bo3ExportZoneSounds(FastFilePath(), C.KindDir(DiskGame::Kind::Sound), C.NameFilter,
                                                   AudioFormat, Report, &Files);
        Kt.Written += Written;
        Kt.Seen += Written;
        if (!SoundAliasCsv::Wanted(C)) return;
        // the zone's SndBank aliases, their file specs pointing at the files just written
        const std::string Extension = ExportWriters::SoundExtension(Bo3SoundFormat(AudioFormat), ".flac");
        SoundAliasCsv::Rows Rows;
        for (const auto& I : Walk().Instances)
            if (I.Parent < 0 && OwnerIs(I, "sound")) Bo3ReadSoundAliases(AliasView(), I.Pos, Files, Extension, Rows);
        SoundAliasCsv::Write(C, T, Name(), Rows);
    }

    // RawFile (47) and ScriptParseTree (54, compiled GSC/CSC bytecode): a name, a size and a
    // payload, 0x18 bytes, written by Bo3WriteRawfilePayload (GSC decompiled via acts, or the bytes).
    void T7Zone::ExportRawfiles(const DiskGame::Context& C, DiskGame::Totals& T)
    {
        auto& Kt = T.Kinds[DiskGame::Kind::Rawfile];
        const std::string Dir = C.KindDir(DiskGame::Kind::Rawfile);
        size_t Decompiled = 0, DecompileFailed = 0, WriteFailed = 0;

        std::vector<const ZoneTables::ZoneResult::Instance*> Raws;
        for (const auto& I : Walk().Instances)
            if (I.Parent < 0 && (OwnerIs(I, "rawfile") || OwnerIs(I, "scriptparsetree"))) Raws.push_back(&I);
        for (size_t N = 0; N < Raws.size(); N++)
        {
            const auto& I = *Raws[N];
            const std::string Nm = Text(I.Pos);
            if (!C.Advance(N, Raws.size(), Nm)) break;
            if (!C.Wants(DiskGame::Kind::Rawfile, Nm)) continue;
            const uint32_t Size = At<uint32_t>(Zone(), I.Pos + 0x8);
            if (Nm.empty() || !Marked(I.Pos + 0x10) || Size == 0) { Kt.References++; continue; }

            const int64_t Payload = Landed(I.Pos + 0x10);
            if (Payload < 0 || (uint64_t)Payload + Size > Zone().size())
            {
                T.Fail(Name(), DiskGame::Kind::Rawfile, Nm, "payload out of range");
                continue;
            }
            Kt.Seen++;
            if (Bo3WriteRawfilePayload(Zone().data() + Payload, Size, Nm, Dir, ActsExe, Decompiled, DecompileFailed, WriteFailed))
                Kt.Written++;
            else
                T.Fail(Name(), DiskGame::Kind::Rawfile, Nm, "write failed");
        }
        Counter("rawfiles_decompiled") += Decompiled;
        Counter("rawfiles_decompile_failed") += DecompileFailed;
        Counter("rawfiles_write_failed") += WriteFailed;
    }

    // Material (6): name, technique-set name, texture slots (Bo3WriteMaterialJson). TechniqueSet
    // accepts a resident (already-loaded) reference too; a texture slot counts as Inline only when
    // the marker is exact: a resident texture slot is an unnamed reference, not this material's own.
    void T7Zone::ExportMaterials(const DiskGame::Context& C, DiskGame::Totals& T)
    {
        auto& Kt = T.Kinds[DiskGame::Kind::Data];
        const std::string Dir = (fs::path(C.KindDir(DiskGame::Kind::Data)) / "materials").string();
        // Not C.KindDir(Image): it creates the folder, so a run without images would report an empty
        // "images" dir as present.
        const std::string ImagesDirNoCreate = (fs::path(C.OutRoot) / DiskGame::KindFolder(DiskGame::Kind::Image)).string();
        size_t TexturesTotal = 0, TexturesResolved = 0, TexturesWithPng = 0;
        std::set<std::string> Seen;

        for (const auto& I : Walk().Instances)
        {
            if (I.Parent >= 0 || !OwnerIs(I, "material")) continue;
            if (!C.Advance(DataDone++, DataCount)) break;
            const std::string Nm = Text(I.Pos);
            if (!C.Wants(DiskGame::Kind::Data, Nm)) continue;
            if (Nm.empty()) { Kt.References++; continue; }
            // The asset list can carry the same shared material more than once: write it once.
            if (!Seen.insert(Nm).second) { Kt.Known++; continue; }
            Kt.Seen++;

            Bo3MaterialCapture Mat;
            const uint8_t TexCount = At<uint8_t>(Zone(), I.Pos + 0x270);
            if (MarkedOrResident(I.Pos + 0x278))
            {
                const int64_t TsPos = Landed(I.Pos + 0x278);
                if (TsPos >= 0) Mat.TechniqueSetName = Text(TsPos);
            }
            if (Marked(I.Pos + 0x280))
            {
                const int64_t TexArr = Landed(I.Pos + 0x280);
                for (uint8_t Ti = 0; TexArr >= 0 && Ti < TexCount; Ti++)
                {
                    const int64_t EntryPos = TexArr + (int64_t)Ti * 0x20;
                    const uint32_t SemanticHash = At<uint32_t>(Zone(), EntryPos + 8);
                    if (Marked(EntryPos))
                    {
                        const int64_t ImgPos = Landed(EntryPos);
                        Mat.Textures.push_back({ SemanticHash, ImgPos >= 0 ? Text(ImgPos + ImageNameAt) : std::string(), true });
                    }
                    else if (Raw(EntryPos) != 0)
                        Mat.Textures.push_back({ SemanticHash, std::string(), false });
                }
            }

            if (Bo3WriteMaterialJson(Nm, Mat, ImagesDirNoCreate, Dir, TexturesTotal, TexturesResolved, TexturesWithPng))
                Kt.Written++;
            else
                T.Fail(Name(), DiskGame::Kind::Data, Nm, "write failed");
        }
        Counter("material_textures_total") += TexturesTotal;
        Counter("material_textures_resolved") += TexturesResolved;
        Counter("material_textures_with_png") += TexturesWithPng;
    }

    // StringTable (48): Columns x Rows grid of deferred cell strings, no real column names.
    void T7Zone::ExportStringTables(const DiskGame::Context& C, DiskGame::Totals& T)
    {
        auto& Kt = T.Kinds[DiskGame::Kind::Data];
        const std::string Dir = C.KindDir(DiskGame::Kind::Data);
        size_t CellsTotal = 0;

        for (const auto& I : Walk().Instances)
        {
            if (I.Parent >= 0 || !OwnerIs(I, "stringtable")) continue;
            if (!C.Advance(DataDone++, DataCount)) break;
            const std::string Nm = Text(I.Pos);
            if (!C.Wants(DiskGame::Kind::Data, Nm)) continue;
            if (Nm.empty()) { Kt.References++; continue; }
            Kt.Seen++;

            Bo3StringTableCapture Cap;
            Cap.Columns = At<uint32_t>(Zone(), I.Pos + 0x8);
            Cap.Rows = At<uint32_t>(Zone(), I.Pos + 0xc);
            if (Marked(I.Pos + 0x10))
            {
                const int64_t Arr = Landed(I.Pos + 0x10);
                const uint64_t Cells = (uint64_t)Cap.Columns * Cap.Rows;
                if (Arr >= 0 && Cells <= 1u << 24)
                {
                    Cap.Cells.resize((size_t)Cells);
                    for (uint64_t Ci = 0; Ci < Cells; Ci++)
                        Cap.Cells[(size_t)Ci] = Text(Arr + (int64_t)Ci * 16);
                }
            }
            CellsTotal += Cap.Cells.size();
            if (Bo3WriteStringTableCsv(Nm, Cap, Dir)) Kt.Written++;
            else T.Fail(Name(), DiskGame::Kind::Data, Nm, "write failed");
        }
        Counter("stringtable_cells_total") += CellsTotal;
    }

    // StructuredTable (49): a real header row plus row/column cells (Type/Text/Number per cell).
    void T7Zone::ExportStructuredTables(const DiskGame::Context& C, DiskGame::Totals& T)
    {
        auto& Kt = T.Kinds[DiskGame::Kind::Data];
        const std::string Dir = C.KindDir(DiskGame::Kind::Data);
        size_t CellsTotal = 0;

        for (const auto& I : Walk().Instances)
        {
            if (I.Parent >= 0 || !OwnerIs(I, "structuredtable")) continue;
            if (!C.Advance(DataDone++, DataCount)) break;
            const std::string Nm = Text(I.Pos);
            if (!C.Wants(DiskGame::Kind::Data, Nm)) continue;
            if (Nm.empty()) { Kt.References++; continue; }
            Kt.Seen++;

            Bo3StructuredTableCapture Cap;
            Cap.CellCount = (int32_t)At<uint32_t>(Zone(), I.Pos + 0x8);
            Cap.ColumnCount = (int32_t)At<uint32_t>(Zone(), I.Pos + 0xc);
            Cap.RowCount = (int32_t)At<uint32_t>(Zone(), I.Pos + 0x10);
            if (Marked(I.Pos + 0x18) && Cap.CellCount >= 0 && Cap.CellCount <= (1 << 24))
            {
                const int64_t Arr = Landed(I.Pos + 0x18);
                if (Arr >= 0)
                {
                    Cap.Cells.resize((size_t)Cap.CellCount);
                    for (int32_t Ci = 0; Ci < Cap.CellCount; Ci++)
                    {
                        const int64_t EntryPos = Arr + (int64_t)Ci * 24;
                        auto& Cell = Cap.Cells[(size_t)Ci];
                        Cell.Type = (int32_t)At<uint32_t>(Zone(), EntryPos);
                        Cell.Number = (int32_t)At<uint32_t>(Zone(), EntryPos + 0x10);
                        Cell.Text = Text(EntryPos + 8);
                    }
                }
            }
            if (Marked(I.Pos + 0x28) && Cap.ColumnCount >= 0 && Cap.ColumnCount <= (1 << 20))
            {
                const int64_t Arr = Landed(I.Pos + 0x28);
                if (Arr >= 0)
                {
                    Cap.Headers.resize((size_t)Cap.ColumnCount);
                    for (int32_t Hi = 0; Hi < Cap.ColumnCount; Hi++)
                        Cap.Headers[(size_t)Hi] = Text(Arr + (int64_t)Hi * 16);
                }
            }
            CellsTotal += Cap.Cells.size();
            if (Bo3WriteStructuredTableCsv(Nm, Cap, Dir)) Kt.Written++;
            else T.Fail(Name(), DiskGame::Kind::Data, Nm, "write failed");
        }
        Counter("structuredtable_cells_total") += CellsTotal;
    }

    void T7Zone::ReadDdlStructs(int64_t Arr, uint32_t Count, std::vector<Bo3DdlStructDefCapture>& Out) const
    {
        Out.resize(Count);
        for (uint32_t Si = 0; Si < Count; Si++)
        {
            const int64_t EntryPos = Arr + (int64_t)Si * 0x38;
            auto& S = Out[Si];
            S.Name = Text(EntryPos);
            const uint32_t MemberCount = At<uint32_t>(Zone(), EntryPos + 0xc);
            if (Raw(EntryPos + 0x10) != 0 && MemberCount <= 65536)
            {
                const int64_t MArr = Landed(EntryPos + 0x10);
                if (MArr >= 0)
                {
                    S.Members.resize(MemberCount);
                    for (uint32_t Mi = 0; Mi < MemberCount; Mi++)
                        S.Members[Mi].Name = Text(MArr + (int64_t)Mi * 0x48);
                }
            }
        }
    }

    void T7Zone::ReadDdlEnums(int64_t Arr, uint32_t Count, std::vector<Bo3DdlEnumDefCapture>& Out) const
    {
        Out.resize(Count);
        for (uint32_t Ei = 0; Ei < Count; Ei++)
        {
            const int64_t EntryPos = Arr + (int64_t)Ei * 0x28;
            auto& E = Out[Ei];
            E.Name = Text(EntryPos);
            const uint32_t MemberCount = At<uint32_t>(Zone(), EntryPos + 0x8);
            if (Raw(EntryPos + 0x10) != 0 && MemberCount <= 65536)
            {
                const int64_t MArr = Landed(EntryPos + 0x10);
                if (MArr >= 0)
                {
                    E.Members.resize(MemberCount);
                    for (uint32_t Mi = 0; Mi < MemberCount; Mi++)
                        E.Members[Mi] = Text(MArr + (int64_t)Mi * 8);
                }
            }
        }
    }

    // Ddl (51): a chain of struct/enum revisions (NextPtr@0x40, any real BO3 schema is 1-2 deep).
    void T7Zone::ExportDdl(const DiskGame::Context& C, DiskGame::Totals& T)
    {
        auto& Kt = T.Kinds[DiskGame::Kind::Data];
        const std::string Dir = C.KindDir(DiskGame::Kind::Data);
        size_t StructsTotal = 0, EnumsTotal = 0;

        for (const auto& I : Walk().Instances)
        {
            if (I.Parent >= 0 || !OwnerIs(I, "ddl")) continue;
            if (!C.Advance(DataDone++, DataCount)) break;
            const std::string Nm = Text(I.Pos);
            if (!C.Wants(DiskGame::Kind::Data, Nm)) continue;
            if (Nm.empty()) { Kt.References++; continue; }
            Kt.Seen++;

            Bo3DdlCapture Cap;
            int64_t NodePos = Raw(I.Pos + 0x8) != 0 ? Landed(I.Pos + 0x8) : -1;
            for (int Guard = 0; NodePos >= 0 && Guard < 4096; Guard++)
            {
                Cap.Chain.emplace_back();
                auto& Node = Cap.Chain.back();

                const uint32_t StructCount = At<uint32_t>(Zone(), NodePos + 0x28);
                if (Raw(NodePos + 0x20) != 0 && StructCount <= 65536)
                {
                    const int64_t Arr = Landed(NodePos + 0x20);
                    if (Arr >= 0) ReadDdlStructs(Arr, StructCount, Node.Structs);
                }
                const uint32_t EnumCount = At<uint32_t>(Zone(), NodePos + 0x38);
                if (Raw(NodePos + 0x30) != 0 && EnumCount <= 65536)
                {
                    const int64_t Arr = Landed(NodePos + 0x30);
                    if (Arr >= 0) ReadDdlEnums(Arr, EnumCount, Node.Enums);
                }
                NodePos = Raw(NodePos + 0x40) != 0 ? Landed(NodePos + 0x40) : -1;
            }

            if (Bo3WriteDdlJson(Nm, Cap, Dir, StructsTotal, EnumsTotal)) Kt.Written++;
            else T.Fail(Name(), DiskGame::Kind::Data, Nm, "write failed");
        }
        Counter("ddl_structs_total") += StructsTotal;
        Counter("ddl_enums_total") += EnumsTotal;
    }

    void T7Zone::ExportData(const DiskGame::Context& C, DiskGame::Totals& T)
    {
        // one progress count over the four
        DataDone = DataCount = 0;
        for (const auto& I : Walk().Instances)
            if (I.Parent < 0)
                for (const char* Type : { "material", "stringtable", "structuredtable", "ddl" })
                    if (OwnerIs(I, Type)) DataCount++;
        ExportMaterials(C, T);
        ExportStringTables(C, T);
        ExportStructuredTables(C, T);
        ExportDdl(C, T);
    }

    // GfxWorld (gfx_map, 0x2040; the t7 table): the surface count at 0x18, GfxWorldDraw at 0x258
    // (vertex count at 0x268, the two vertex streams at 0x270 and 0x288, u16 indices at 0x2a0),
    // the brush models at 0x2b8 (64 bytes) and GfxWorldDpvsStatic at 0x2f8 (surfaces at 0x430,
    // static model instances at 0x438, 0x98 each, their count at 0x2fc).
    constexpr int64_t WorldName = 0, WorldBaseName = 8, WorldSurfaceCount = 0x18;
    constexpr int64_t WorldVertexCount = 0x268, WorldPositions = 0x270, WorldVertexData = 0x288;
    constexpr int64_t WorldIndexCount = 0x298, WorldIndices = 0x2a0, WorldModelCount = 0x2b0, WorldModels = 0x2b8;
    constexpr int64_t WorldSurfaces = 0x430, WorldSModelCount = 0x2fc, WorldSModels = 0x438, SModelSize = 0x98, SModelModel = 0x58;
    // ComWorld: primary lights (564 bytes each); clipMap_t: two dynamic entity lists (0x98 each)
    constexpr int64_t ComLightCount = 0xc, ComLights = 0x10;
    // The lighting states (0x11b8 each, t7 table f13fe860): four 0xf4-byte records whose sky model
    // is an inline name at 0x6c.
    constexpr int64_t WorldStateCount = 0xe8, WorldStates = 0xf0, StateSkyName = 0x6c, StateSkyStride = 0xf4;
    constexpr int64_t ClipDynCount = 0x1ba, ClipDyns = 0x1c8, DynSize = 0x98;

    bool T7Zone::ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models)
    {
        const int64_t World = Root("gfx_map");
        if (World < 0) return false;
        const auto& Img = Image();
        Scene.Name = StringAt(At<uint64_t>(Img, World + WorldBaseName));
        if (Scene.Name.empty()) Scene.Name = StringAt(At<uint64_t>(Img, World + WorldName));
        if (Scene.Name.empty()) Scene.Name = Name();
        const auto Model = [this](uint64_t Address) { return ModelAt(Address); };
        FILE* Dbg = Maps::DebugFile();

        // ---- world: surfaces give their first vertex as a byte offset into the 12-byte positions
        // (at 12), their triangle count at 42 and first index at 44; brushes their surface count
        // at 56 and first surface at 60.
        const uint32_t VertexCount = At<uint32_t>(Img, World + WorldVertexCount);
        const int64_t Positions = Off(At<uint64_t>(Img, World + WorldPositions)), Data = Off(At<uint64_t>(Img, World + WorldVertexData));
        Maps::WorldLayout L;
        L.Image = &Img;
        L.Surfaces = Off(At<uint64_t>(Img, World + WorldSurfaces));
        L.SurfaceCount = At<uint32_t>(Img, World + WorldSurfaceCount);
        L.Brushes = Off(At<uint64_t>(Img, World + WorldModels));
        L.BrushCount = At<uint32_t>(Img, World + WorldModelCount);
        L.Indices = Off(At<uint64_t>(Img, World + WorldIndices));
        L.IndexCount = At<uint32_t>(Img, World + WorldIndexCount);
        L.SurfaceSize = Treyarch::SurfaceSize, L.FirstVertex = 12, L.FirstVertexUnit = 12, L.TriCount = 42, L.BaseIndex = 44;
        L.Material = Treyarch::SurfaceMaterial;
        L.BrushSize = 64, L.FirstSurface = 60, L.SurfaceCountAt = 56;
        if (Dbg)
            std::fprintf(Dbg, "[map] %s vertices %u at %llx/%llx indices %u at %llx surfaces %u at %llx brushes %u at %llx\n",
                         Scene.Name.c_str(), VertexCount, (long long)Positions, (long long)Data, L.IndexCount, (long long)L.Indices,
                         L.SurfaceCount, (long long)L.Surfaces, L.BrushCount, (long long)L.Brushes);
        const auto MaterialOf = [&](uint64_t Address) {
            return Off(Address) < 0 ? std::string() : Maps::AddMaterial(Scene, GameBlackOps3::ReadXMaterial(Address));
        };
        if (const size_t Bad = Maps::AddWorld(Scene, L, Treyarch::WorldVertices(Img, Positions, Data, VertexCount), MaterialOf))
            Counter("map_triangles_unread") += Bad;

        AddStaticModels(Scene, Models, Off(At<uint64_t>(Img, World + WorldSModels)),
                        At<uint32_t>(Img, World + WorldSModelCount), SModelSize, SModelModel);

        // ---- dynamic entities (the clip map's two lists), primary lights and the entity string
        if (const int64_t Clip = Root("col_map"); Clip >= 0)
            for (int List = 0; List < 2; List++)
                Treyarch::DynamicEntities(Img, Off(At<uint64_t>(Img, Clip + ClipDyns + List * 8)), At<uint16_t>(Img, Clip + ClipDynCount + List * 2),
                                          DynSize, "dynent_" + std::to_string(List) + "_", Model, Scene, Models);
        if (const int64_t Com = Root("com_map"); Com >= 0)
            Treyarch::Lights(Img, Off(At<uint64_t>(Img, Com + ComLights)), At<uint32_t>(Img, Com + ComLightCount),
                             { 564, 88, 156, 268, 108 }, Scene);
        AddMapEnts(Scene);
        // clipMap_t's brushes (the t7 tables' col_map):
        // the world's ClipInfo at 0x10 (0x90: ClipMaterial 0x10 at 0x10/0x18, name, surface 8, contents
        // 0xc; leaf brush nodes 0x30/0x38, 0x18 each; brushes u16 0x70, records 0x78); cbrush_t 0x70
        // (mins, contents 0xc, maxs 0x10, numsides 0x1c, sides 0x20, axial cflags 0x28 and sflags
        // 0x40); cbrushside_t 0x10 (plane, cflags 8, sflags 0xc); brush models 0x120/0x128, cmodel_t
        // 0x60: a ClipInfo pointer at 0x20 (null in the zones seen), its leaf (0x28) leafBrushNode at 0x50
        {
            Maps::ClipLayout C;
            C.InfoAt = 0x10;
            C.Info.MaterialCount = 0x10, C.Info.Materials = 0x18, C.Info.BrushCount = 0x70, C.Info.Brushes = 0x78;
            C.Info.LeafBrushNodeCount = 0x30, C.Info.LeafBrushNodes = 0x38;
            C.MaterialSize = 0x10, C.MaterialName = 0, C.MaterialSurface = 8, C.MaterialContents = 0xc;
            C.BrushSize = 0x70, C.BrushMins = 0, C.BrushContents = 0xc, C.BrushMaxs = 0x10, C.BrushSideCount = 0x1c, C.BrushSides = 0x20;
            C.AxialContents = 0x28, C.AxialSurface = 0x40;
            C.SideSize = 0x10, C.SidePlane = 0, C.SideContents = 8, C.SideSurface = 0xc;
            C.ModelCount = 0x120, C.Models = 0x128, C.ModelSize = 0x60, C.ModelInfo = 0x20, C.ModelLeafBrushNode = 0x50;
            C.NodeSize = 0x18;
            C.VertexCount = 0xd8, C.Vertices = 0xe0, C.TriangleCount = 0xe8, C.Triangles = 0xf0, C.TriangleIndexBytes = 4;
            AddCollision(Scene, { "col_map" }, C);
            AddTriggers(Scene, 0x18);   // MapEnts' MapTriggers
        }

        // ---- the sky: the first named in the first lighting state, a model of this zone found by name
        const int64_t State = Off(At<uint64_t>(Img, World + WorldStates));
        for (int K = 0; State >= 0 && At<uint32_t>(Img, World + WorldStateCount) > 0 && K < 4 && Scene.Sky.Model.empty(); K++)
            Scene.Sky.Model = Maps::InlineName(Img, State + StateSkyName + K * StateSkyStride, 64);
        if (!Scene.Sky.Model.empty())
            for (const auto& I : Walk().Instances)
                if (I.Parent < 0 && OwnerIs(I, ModelType()) && ModelAt(VirtualGameSession::Base + I.Pos) == Scene.Sky.Model)
                {
                    Models.emplace(Scene.Sky.Model, VirtualGameSession::Base + I.Pos);
                    break;
                }
        return true;
    }

    class T7Game final : public DiskGame::IDiskGame
    {
    public:
        const char* Id() const override { return "bo3"; }
        const char* Title() const override { return "Call of Duty: Black Ops III"; }
        const char* Engine() const override { return "T7"; }
        std::vector<std::string> Aliases() const override { return { "t7" }; }
        std::unique_ptr<DiskGame::IDiskZone> Unopened() const override
        {
            return std::make_unique<T7Zone>(std::string(), std::string(), std::string());
        }
        // the patch's strings: multiplayer maps by MPUI_ or their DLC's (mp_biodome "Aquarium",
        // mp_aerospace DLC2_ "Spire"), the campaign by CPUI_, zombies by their DLC's, ZMUI_ or
        // MPUI_ (zm_factory "The Giant"); many keys have no text in this build
        const LevelNaming* LevelNames() const override
        {
            static const LevelNaming Naming{
                { "??_core_patch", "??_mp_patch", "??_zm_patch", "??_cp_patch" },
                { { "mp_*", { "MPUI_{REST}", "DLC1_{REST}", "DLC2_{REST}", "DLC3_{REST}", "DLC4_{REST}" } },
                  { "cp_*", { "CPUI_{NAME}" } },
                  { "zm_*", { "DLC5_{REST}", "DLC4_{REST}", "DLC3_{REST}", "DLC2_{REST}", "DLC1_{REST}", "ZMUI_{REST}",
                              "MPUI_{REST}", "DLC5_PRESENCE_{NAME}", "PLAYLIST_CATEGORY_{NAME}" } } } };
            return &Naming;
        }

        std::unique_ptr<DiskGame::IDiskZone> Open(const std::string& Path, const DiskGame::ExportOptions& Options,
                                                  std::string& Error) const override
        {
            auto Zone = std::make_unique<T7Zone>(Path, Options.AudioFormat, Options.ActsExe);
            if (!Options.Profile.empty()) Zone->SetProfile(Options.Profile);
            if (!Zone->Open(Error)) return nullptr;
            return Zone;
        }
    };

    const T7Game TheGame;
    const DiskGame::Registration Registered(TheGame);
}
