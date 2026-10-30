#pragma once

// IDiskZone for every game read by the zonekit table walker (BO4, Ghosts, and the next ones).
// Open() does what is the same for all of them: load the zone, walk it with the game's profile
// (layout on), serve it relocated as the game's memory (VirtualGameSession) with every pointer
// linked - inline data, asset-list references, data back references through the stream layout,
// and asset pointers followed the way resolve_deref does - then ask the game for its fixups.
// Models and animations then go through the game's own live readers (ExportWriters). A game
// supplies the hooks below; images and sounds, whose storage differs per game, it Exports() itself.

#include "ExportNaming.h"
#include "ExportWriters.h"
#include "ICoDGame.h"
#include "MapCollision.h"
#include "MapScene.h"
#include "ScriptExport.h"
#include "SoundAliasCsv.h"
#include "VirtualGameSession.h"

#include <functional>
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <unordered_map>

class CoDPackageCache;
namespace Techsets
{
    class Format;
}
namespace Gdt
{
    class Type;
    struct Memory;
}
namespace Treyarch
{
    struct EffectZone;
    enum class EffectLayout;
}

// Every pointer of a layout walk, resolved: inline links, asset-list references, data back
// references through the stream layout, and (DerefAssetPointers) asset pointers followed the way
// resolve_deref does. In application order; Counters say how many of each kind and how many stay
// unresolved. What ZonekitZone serves the live code, and what any other reader of a zone uses.
struct ResolvedLinks
{
    std::vector<std::pair<int64_t, int64_t>> Links;   // pointer field -> where its data is
    std::map<std::string, size_t> Counters;           // links_inline/asset/data/deref, refs_unresolved
};
// What resolving a partial walk's links (ZoneTables::PartialWalk::Merged) takes from the zone's
// resume points instead of the reads it lacks: where each listed asset starts and its cursors there,
// the resident slots as the walks reserved them, the zone's keys and its assets by hash
struct ResolveHints
{
    const std::vector<ZoneTables::ZoneResult::ResumePoint>* Points = nullptr;
    const std::vector<std::pair<uint64_t, int64_t>>* Keys = nullptr;               // every key of the zone, in stream order
    const std::map<std::pair<std::string, uint64_t>, int64_t>* ByHash = nullptr;   // (type name, "" any; hash) -> asset
    // Keys by key, the first of each (the filing the game's map keeps): built on the first use and
    // kept, a partial walk's rounds and links asking it again and again (millions of keys)
    const std::unordered_map<uint64_t, int64_t>& Keyed() const;

private:
    mutable std::shared_ptr<const std::unordered_map<uint64_t, int64_t>> KeyedOnce;
};
ResolvedLinks ResolveLinks(const ZoneTables::ZoneResult& R, const ZoneTables::StreamRules& Rules, bool DerefAssetPointers,
                           ByteView Zone = {}, const ResolveHints* Hints = nullptr);
// The resume points a partial walk's links reach that it has not walked yet: the owners of its back
// references' targets (data, slots, listed assets, keys, hashes) and of its inline links' targets
std::set<size_t> MissingOwners(const ZoneTables::ZoneResult& M, const ZoneTables::StreamRules& Rules,
                               const ZoneTables::PartialWalk& Walk, const ResolveHints& Hints);
// Targets walked, then every resume point their links reach (MissingOwners), round after round;
// Seen: the walks a round has looked at already (kept between calls). False when a walk fails or
// more than AtMost points are walked. On the walker's stack (ZoneTables::WalkOnLargeStack).
bool WalkReached(ZoneTables::PartialWalk& Walk, const std::vector<size_t>& Targets, const ZoneTables::StreamRules& Rules,
                 const ResolveHints& Hints, std::set<size_t>& Seen, size_t& Rounds, size_t AtMost = 4000);

// What a walk with this profile depends on (its tables, its stream layout), hashed: the key of
// anything kept from one (the install index, BO4's material books).
std::string ProfileKey(const ExportWriters::LoadedProfile& Loaded);

// A fast file's zone walked by the zonekit tables of its game (DiskGame::Detect), the layout on:
// what the commands reporting on one zone read (decompress-ff).
struct ProfileWalk
{
    std::string Profile;              // the zonekit profile walked with
    int PointerSize = 8;              // its pointer fields' width
    ZoneTables::ZoneResult R;
    size_t Reached = 0;               // the zone-level assets the walk entered
    size_t Walked = 0;                // ... and read whole
    bool Complete = false;            // walked to the end of the zone
    std::unordered_map<int64_t, int64_t> Links;   // inline pointer field -> where its data is

    // The text an inline string pointer at Field names, "" when it is not one
    std::string String(ByteView Zone, int64_t Field) const;
};
// False with Error when the file's game is not read by zonekit tables or its profile does not load
bool WalkWithProfile(const std::string& FastFilePath, ByteView Zone, ProfileWalk& Out,
                     std::string& Error);

// A script bundle value's kind (ZonekitZone::ScriptBundleLayout)
enum class BundleValue : uint8_t { Raw, Bool, Int, Float, Localize, String, Struct, KeyedStruct, Array, Anim, Hash, Asset };

class ZonekitZone : public DiskGame::IDiskZone
{
public:
    explicit ZonekitZone(std::string FastFilePath);
    ~ZonekitZone() override;

    // Another zonekit profile than Profile() (--profile), set before Open.
    void SetProfile(const std::string& Name) { ProfileName = Name; }
    // The profile Open walks with
    std::string ProfileUsed() const { return ProfileName.empty() ? Profile() : ProfileName; }
    // Packages the zone holds and serves as CoDAssets' package cache whenever it is served (the
    // live readers stream mesh, pixel and sound data from there), set before Open
    void ServesPackages(std::shared_ptr<CoDPackageCache> Packages) { Held = std::move(Packages); }
    // Loads, walks and relocates; false with Error when the zone does not read. Inside a
    // DiskGame::PartialScope, a zone that OpensPartially walks only the assets asked and what they
    // reach, from its resume index (ResumeIndex.h); without the index, or when the walk falls short,
    // the whole zone, which writes the index for the next time.
    bool Open(std::string& Error);
    bool OpenedPartially() const override { return Partial; }

    // How far a partial open goes: at most Assets asked, at most Walks assets walked for them (what
    // they reach included, the asked first); past either, the zone is read whole. What a walk of
    // what is asked costs is what it reaches: a few assets may reach most of the zone.
    struct PartialLimits
    {
        size_t Assets = 256;
        size_t Walks = 4000;
    };
    virtual PartialLimits Partially() const { return {}; }

private:
    bool OpenPartially(const DiskGame::Selection& Want, const std::string& File, const std::function<void(const char*)>& Lap);
    // Want only images the resume index File holds whole (Heads): served from their records alone,
    // the zone neither read nor walked; false otherwise
    bool OpenFromHeads(const DiskGame::Selection& Want, const std::string& File, const std::function<void(const char*)>& Lap);
    void WriteResume(const std::string& File) const;
    // the heads the game keeps (Heads) are beside the resume index File, or it keeps none
    bool HeadsKept(const std::string& ResumeFile) const;
    // one of Assets is among those the zone defines (DefinesAt)
    bool DefinesAny(const std::vector<DiskGame::AssetId>& Assets) const;
    std::vector<DiskGame::AssetId> Unlisted;   // asked of a partial open, not in its resume index

public:
    // the full walk and what it walked (the bytes before relocation), its profile's tables and type
    // names, its stream rules (LayoutOffset applied): what a partial walk of the zone needs
    const ZoneTables::ZoneResult& FullWalk() const { return R; }
    bool ResolvesDerefs() const { return DerefAssetPointers(); }
    ByteView WalkedBytes() const { return Bytes; }
    const ExportWriters::LoadedProfile* WalkProfile() const { return Loaded; }
    ZoneTables::StreamRules WalkRules()
    {
        ZoneTables::StreamRules Rs = Loaded->Profile.Rules;
        Rs.StartOffset = LayoutOffset();
        return Rs;
    }

    const std::string& Name() const override { return ZoneName; }
    bool WalkedToEnd() const override { return Walked; }
    void Measures(DiskGame::Totals& T) const override;
    // DefinesAt's assets (the install index's)
    void Defines(std::vector<DiskGame::AssetId>& Out) const final;
    // with each one's weight: the bytes its instance's reads hold in the zone (an asset inlined in
    // it that the index weighs on its own, or that another asset's kind is, left out), then Weigh
    void Weighs(std::vector<std::pair<DiskGame::AssetId, DiskGame::Weight>>& Out) const final;
    // the entries of its LocalizeType() assets, by LocalizeEntry
    void Strings(std::vector<std::pair<std::string, std::string>>& Out) const override;
    // its stringtable assets: name at 0, column and row counts, the cells (a string pointer each)
    // after them, laid out by the pointer width
    void Documents(std::vector<std::pair<std::string, std::string>>& Out) const override;
    std::string IndexKey() const override;   // the profile's files, hashed
    uint64_t Footprint() const override;
    void OpenTimes(std::vector<std::pair<std::string, double>>& Out) const override { Out = Steps; }
    // Parked: the session ended (its lock freed), the relocated zone kept; Resume serves it again.
    bool Park() override;
    void Resume() override;
    // its MaterialType materials' images through ReadsMaterials' reader (ZonekitZone.Materials.cpp)
    bool MaterialImages(DiskGame::MaterialBook& Out) override;
    // Visit each zone-level asset of the profile's gdt type Id (<profile>/gdt/<Id>/) with the type
    // and how it reads the zone (Gdt::Type::Values); false and Error when the profile has no such type
    bool ForEachGdtAsset(const std::string& Id, const std::function<void(const Gdt::Type&, const Gdt::Memory&, int64_t Pos)>& Visit,
                         std::string& Error) const;

protected:
    // What the game must say
    virtual const char* Profile() const = 0;                 // zonekit profile name
    virtual const ICoDGame& LiveGame() const = 0;            // whose live readers serve the zone
    virtual VirtualGameSession::Handlers Handlers() = 0;     // its string and image handlers

    // What the game may change
    virtual bool LoadBytes(ZoneBytes& Zone, std::string& Error);   // FastFile::Decompress
    // the files LoadBytes reads (a resume index is kept for them as they are: path, size, time
    // written); none when it reads from inside an archive (the index is then kept for the bytes)
    virtual std::vector<std::string> SourceFiles() const { return { FastFilePath() }; }
    // IW6: an ASSET pointer's back reference names the pointer field to follow (resolve_deref)
    virtual bool DerefAssetPointers() const { return false; }
    // before the walk (the profile loaded): where in its start block the zone's reads begin, the
    // game having loaded something there first (Infinite Warfare: the shared stream, in block 8)
    virtual int64_t LayoutOffset() { return 0; }
    // after relocation: what the game's load-time fixups write (Ghosts: LOD surfaces)
    virtual void Fixups() {}
    // a game whose readers and Fixups work on a partial walk's result (only the walked assets in it)
    virtual bool OpensPartially() const { return false; }
    // the types whose instances the game counts over the whole zone (a streamed image's records
    // are the stream table's by its rank among them): a resume index keeps where each one is, for
    // AllOf after a partial open
    virtual std::vector<std::string> Ranked() const { return {}; }
    // the assets this zone holds whole, each with where it is (its instance's position; -1 for one
    // a resume index cannot name): what the install index lists and a resume index finds. The
    // models, animations and materials; a game adds its own kinds
    virtual void DefinesAt(std::vector<std::pair<DiskGame::AssetId, int64_t>>& Out) const;
    // what the game's reader knows of the asset at Pos beyond its bytes in the zone (W.Bytes, set):
    // its data streamed from packages (added), its pixels or samples (DiskGame::Weight)
    virtual void Weigh(DiskGame::Kind K, int64_t Pos, DiskGame::Weight& W) const {}
    // the zone becomes the one the game's plain handler functions reach (On: opened or resumed),
    // or stops being it (parked)
    virtual void Served(bool On) {}
    // an asset's name: by default the string its first field points to
    virtual std::string AssetName(DiskGame::Kind K, int64_t Pos) const { return StringAt(PointerAt(Pos)); }
    // a name-only header, defined in another zone ("," names by default), and the name of the
    // asset it stands for (without the ",")
    virtual bool IsReference(DiskGame::Kind K, const std::string& Name, int64_t Pos) const { return Name.empty() || Name[0] == ','; }
    virtual std::string Referenced(DiskGame::Kind, const std::string& Name) const
    {
        return !Name.empty() && Name[0] == ',' ? Name.substr(1) : Name;
    }
    // An asset of K the live reader cannot write (WWII's composite models, a list of other
    // models): left out of the export and of Defines, counted under the counter it names
    virtual const char* Unwritable(DiskGame::Kind K, int64_t Pos) const { return nullptr; }
    // Whether AssetName reads the asset's own name (false: a name the walk leaves elsewhere, which
    // the rawfile kind fails rather than write under a wrong one)
    virtual bool NameInPlace(DiskGame::Kind K, int64_t Pos) const { return true; }
    virtual const char* ModelType() const { return "xmodel"; }
    virtual const char* AnimType() const { return "xanim"; }
    virtual const char* ImageType() const { return "image"; }   // what a texture slot points at
    // Whether a texture slot's address is an image of a zone served beside this one (its pixels
    // then pulled by name from the zone that defines it, as any texture this zone lacks)
    virtual bool ImageBeside(uint64_t Address) const { return false; }
    // localized strings: the zone-level type, and an entry's key and text (value at +0, name at
    // +8 by default); an empty key is a reference, defined in another zone
    virtual const char* LocalizeType() const { return "localize"; }
    virtual void LocalizeEntry(int64_t Pos, std::string& Key, std::string& Text) const
    {
        Text = TextAt(Pos);
        Key = StringAt(PointerAt(Pos + Rules().PointerSize));
    }
    // the code page of its strings that are not UTF-8: by the language its path names by default
    // (ExportWriters::LanguageCodePage)
    virtual unsigned LocalizeCodePage() const { return ExportWriters::LanguageCodePage(Path); }
    // how a model file names its textures: <out>/images, seen from models/<name>/
    virtual ExportWriters::ModelTextures Textures(const DiskGame::Context& C) const;
    // The level this zone holds, when it holds one (false otherwise): its world surfaces,
    // lights, placements and sky in Scene, a placement's Model named by ModelAt, and where each
    // placed model is (Models, its address). ExportMap reads the models and writes the rest.
    virtual bool ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models) { return false; }
    // Whether the game draws a material as geometry (false: a shadow caster): ExportMap leaves the
    // placed models' surfaces of the others out
    virtual bool MaterialDrawn(const XMaterial_t& M) const { return true; }

    // For the game's own overrides
    // views kept as members, so a reader may hold their address (a ZoneView)
    const ByteView& Zone() const { return ZoneView = Bytes; }
    const ByteView& Image() const { return ImageView = Session->Image(); }
    const ZoneTables::ZoneResult& Walk() const { return R; }
    const ZoneTables::StreamRules& Rules() const;
    VirtualGameSession& Live() { return *Session; }
    const std::string& FastFilePath() const { return Path; }
    bool OwnerIs(const ZoneTables::ZoneResult::Instance& I, const char* Type) const;
    std::string StringAt(uint64_t Address) const;   // a relocated string pointer's text, "" if outside
    // The text of the string pointer field at Field: relocated, else a back reference the walk left
    // unlinked (a string loader its tables test as an IF chain) found through the stream layout;
    // "" when neither
    std::string TextAt(int64_t Field) const;
    // The model asset at a relocated address, by name (a name-only header's referenced name)
    std::string ModelAt(uint64_t Address) const;
    // The zone-level asset of a type (the first), -1 when the zone has none
    int64_t Root(const char* Type) const;
    // the zone lists an asset of Type: from its asset list, so a partial open (where Root finds
    // only the walked ones) answers as a full one
    bool Lists(const char* Type) const;
    // where every instance of a Ranked type is, in walk order: the walk's, or after a partial open
    // its resume index's (the whole zone)
    std::vector<int64_t> AllOf(const char* Type) const;
    // a partial open's: the whole zone's keys and assets by (type, hash) from its resume index (the
    // games resolving by key and hash), nullptr after a full open (the walk holds them)
    const std::vector<std::pair<uint64_t, int64_t>>* WholeKeys() const { return Partial ? &Whole.Keys : nullptr; }
    const std::map<std::pair<std::string, uint64_t>, int64_t>* WholeHashes() const { return Partial ? &Whole.ByHash : nullptr; }
    // Where a relocated pointer's data is in the zone, -1 when it is elsewhere
    int64_t Off(uint64_t Address) const
    {
        return Session && Session->Contains(Address) ? (int64_t)(Address - Session->Origin()) : -1;
    }
    // The relocated pointer field at a zone offset, the game's pointer width (8, or 4 on x86)
    uint64_t PointerAt(int64_t Pos) const
    {
        return Rules().PointerSize == 4 ? (uint64_t)At<uint32_t>(Image(), Pos) : At<uint64_t>(Image(), Pos);
    }
    // The map kind's exporter, for a game whose ReadMap reads its level: Exports(Kind::Map, ...)
    void ExportMap(const DiskGame::Context& C, DiskGame::Totals& T);
    // For ReadMap: the static model placements of a GfxWorldDpvsStatic array, each record the
    // origin (0), the axis rows (12) and the scale (48) from Placement (0, Black Ops II 4), its
    // model pointer at ModelField (Ghosts 0xb0 records / 0x38, BO3 0x98 / 0x58); named
    // smodel_<k>. And the map's entity string.
    void AddStaticModels(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models, int64_t Array,
                         uint32_t Count, int64_t Stride, int64_t ModelField, int64_t Placement = 0);
    void AddMapEnts(Maps::MapScene& Scene, const char* Type = "map_ents");
    // The clip map's brushes (MapCollision.h) into Scene.Collision: the zone-level asset of the
    // first of Types the zone holds, read with the game's layout. False when there is none.
    bool AddCollision(Maps::MapScene& Scene, const std::vector<const char*>& Types, const Maps::ClipLayout& L);
    // The entity string's trigger volumes (Maps::ReadTriggers): the MapTriggers at At in the map
    // entities AddMapEnts reads (Type)
    bool AddTriggers(Maps::MapScene& Scene, int64_t At, const char* Type = "map_ents", const Maps::TriggerLayout& L = {});
    // The served zone as MapCollision reads it
    Maps::ClipSource ClipSource() const;
    size_t& Counter(const std::string& Name) { return Counters[Name]; }
    // A sound file (File: its path under the sound kind's folder, UTF-8) written as WriteSound
    // writes it in the run's sound format: counted written and under Written, else failed with why.
    // With Sink: Data copied and handed to its pool, the counts held until FinishSoundFiles (the
    // reader's own failures meanwhile through Sink.Locked)
    void WriteSoundFile(const DiskGame::Context& C, DiskGame::Totals& T, const std::string& File, const uint8_t* Data, size_t Size, const char* Written,
                        ExportWriters::SoundSink* Sink = nullptr);
    // Sink's sound files written, what they counted added to the zone's counters
    void FinishSoundFiles(ExportWriters::SoundSink& Sink);
    // The relocated zone as a sound alias reader reads it (SoundAliasCsv::ZoneView)
    SoundAliasCsv::ZoneView AliasView() const
    {
        return { &Image(), [this](uint64_t A) { return Off(A); }, [this](uint64_t A) { return StringAt(A); }, Rules().PointerSize };
    }
    // The N bytes a relocated pointer names, false when they are not all in the zone
    bool BytesAt(uint64_t Address, uint64_t N, std::vector<uint8_t>& Out) const;

    // The scripts kind, for a game whose zones hold scripts (Exports(Kind::Script, ...)): each
    // top-level asset of a type of Readers, named by AssetName, read by its reader (false: not
    // read, a failure) and written through a Scripts::Sink for the game's VM. A reader leaves
    // Out.Name empty for an asset that is not a script (a rawfile of another kind).
    using ScriptReader = std::function<bool(int64_t Pos, Scripts::Script& Out)>;
    void ExportScripts(const DiskGame::Context& C, DiskGame::Totals& T, const Scripts::Vm& Vm,
                       const std::vector<std::pair<const char*, ScriptReader>>& Readers);
    // The image kind of a zone whose images are found by name (an archive's file, else the pixels
    // the zone carries): each image it holds, once per name in name order, decoded by Decode; a
    // "," name no image here has is needed from the zone defining it. Unread says where an image
    // that does not decode was looked for. ReferencesByName: a "," name is first decoded by its
    // name alone (Pos -1: an archive file no zone holds whole, a localized .iwd's), needed from
    // another zone only when it does not.
    using NamedImageDecoder = std::function<std::unique_ptr<XImageDDS>(const std::string& Name, int64_t Pos)>;
    void ExportNamedImages(const DiskGame::Context& C, DiskGame::Totals& T, const NamedImageDecoder& Decode, const char* Unread,
                           bool ReferencesByName = false);
    // Defines' images for such a zone: every named image it holds whole
    void DefineImages(std::vector<std::pair<DiskGame::AssetId, int64_t>>& Out) const;

    // The image kind of a zone whose images the game lists itself (Order: their positions, in the
    // order to write them), each named at its position + NameAt: identical content written once
    // (KeyOf: an ImageSink content key for the extension), a reference ("," name, or one Held says
    // is not here) needed from the zone defining it unless an image here has that name.
    struct ImageList
    {
        std::vector<int64_t> Order;
        int64_t NameAt = 0;
        std::function<uint64_t(int64_t Pos, const std::string& Extension)> KeyOf;
        // the pixels; Name is "" when the sink rebuilds a file it could not link
        std::function<std::unique_ptr<XImageDDS>(int64_t Pos, const std::string& Name)> Decode;
        std::function<bool(int64_t Pos)> Held;             // unset: every image listed is here
        std::function<std::string(int64_t Pos)> FailWhy;   // what a failure says, "" unset
        bool OnePerName = false;     // a later image of a name already written is left out
        bool NamesChecked = false;   // an unnamed image is left out, an unreadable name failed
    };
    void ExportImageList(const DiskGame::Context& C, DiskGame::Totals& T, const ImageList& L);

    // An image a run may serve from the zone's heads alone (ResumeIndex::Heads), the zone not read:
    // its type, its record's size, where its name pointer is (-1: named by a hash in the record), and
    // the data its record points at that its export reads (its pixels when the zone holds them, its
    // mips' table), each kept after the record: a pointer the zone does not resolve is left as it
    // is when null, else the image is not kept, nor is one whose data is past Max bytes. Size 0: the
    // game keeps none.
    // Reads: for a reader that finds an image's data by the walk's reads, not by its pointers
    // (Black Ops 4's DescribeZone): the record as loaded (not relocated) and each later read of
    // its type within it (its size and bytes, ReadsMax bytes in all, else the image is not kept),
    // served as the walk of those images alone records them (ZoneResult's reads); an image whose
    // name another image of the zone has with other bytes is not kept.
    struct HeadSpec
    {
        struct Pointed
        {
            int64_t At = 0;                                   // the pointer's offset in the record
            std::function<uint64_t(ByteView Record)> Bytes;  // how many bytes it points at
            uint64_t Max = ~0ull;
        };
        const char* Type = nullptr;
        int64_t Size = 0;
        int64_t NameAt = -1;
        std::vector<Pointed> Data;
        bool Reads = false;
        uint64_t ReadsMax = 0;
    };
    // the images of IwctxReaders::LoadXImage and ModernIwReaders::LoadMipImage (MWIII, Black Ops 6
    // and 7): a 0x40 record, the hash at 0, its in-zone pixels at 0x38 (u32 size 0x18), its mips'
    // table at 0x30 (24 bytes per mip, u8 count 0x2e)
    static HeadSpec ModernIwHeads();
    // ... and what their readers write of the image at Pos: its pixels in the zone (u32 0x18 bytes
    // at 0x38), else the largest level the packages hold (its mips' table at 0x30, u8 count 0x2e,
    // 24 bytes each {key, pad, packed: the running size from bit 30}), its size u16 0x20 by 0x22
    // halved per level below the last. Dxgi, when given, makes a level's bytes from its size and
    // its format (u8 0x2a) instead (Black Ops 7's packed field is no size)
    DiskGame::Weight ModernIwImageWeight(int64_t Pos, uint32_t (*Dxgi)(uint8_t Format) = nullptr) const;
    virtual HeadSpec Heads() const { return {}; }

    // The gdt kind (Gdt.h), for a game whose profile has gdt/<type>/ folders: each top-level asset
    // of every type's zone type, as BO3 GDT blocks, one <zone>.gdt per zone.
    void ExportsGdt();
    // A sound alias's name by its hash, "" when the zone does not say (the gdt kind's alias fields)
    virtual std::string AliasName(uint32_t Hash) const { return std::string(); }
    // The fx kind, for a game that reads its effects (Exports(Kind::Fx, ...)): each top-level asset
    // of Type, named by AssetName(Kind::Fx) (a reference left out), read by Read into JSON text
    // (false: not read, a failure) and written as fx/<name>.json, the name's folders kept.
    using EffectReader = std::function<bool(int64_t Pos, const std::string& Name, std::string& Json)>;
    void ExportEffects(const DiskGame::Context& C, DiskGame::Totals& T, const char* Type, const EffectReader& Read);
    // The served zone as the effect readers read it (Treyarch::EffectZone); and the material and
    // fx kinds of a game whose effects Treyarch's reader reads with layout L, its materials
    // through the live reader Read (both in Games/Treyarch/TreyarchEffects.cpp)
    Treyarch::EffectZone EffectView() const;
    void ExportsEffects(std::function<XMaterial_t(uint64_t Address)> Read, Treyarch::EffectLayout L);
    // The material kind, for a game that reads its materials (ExportsMaterials): each top-level
    // asset of Type (a reference needed from the zone that defines it), read by Read into JSON
    // text and the names of its images, written as materials/<name>.json, its images by the
    // image kind (ExportTextures). The install index then lists the zone's materials, so an
    // effect's material defined in another zone is pulled from there (C.Needs(Kind::Material)).
    // Indexed false: the zone's materials are complete without another zone's (Black Ops 4's
    // books), the index does not list them.
    using MaterialReader = std::function<bool(const std::string& Name, uint64_t Address, std::string& Json, std::set<std::string>& Images)>;
    void ExportsMaterials(const char* Type, MaterialReader Read, bool Indexed = true);
    // The live material reader: the model kind then gives a name-only material header (a material
    // of Type, ExportsMaterials', this zone or another defines whole) that one's images
    // (Context::MaterialsElsewhere; ExportsEffects sets it). ZonekitZone.Materials.cpp
    void ReadsMaterials(std::function<XMaterial_t(uint64_t Address)> Read) { MaterialOf = std::move(Read); }
    // Images some materials name (a game's own fx kind), as the material kind writes them: by the
    // image kind, those not in the zone needed from the one that defines them
    void ExportMaterialImages(std::set<std::string> Images, const DiskGame::Context& C, DiskGame::Totals& T);

    // The rawfile kind (ZonekitZone.Data.cpp), for a game whose zones hold rawfiles or Lua files
    // (Exports(Kind::Rawfile, ...)): each top-level asset of a type of Readers, its name first
    // AssetName(Kind::Rawfile) (a reference left out), its bytes read by its reader (false: not
    // read, a failure; the reader may complete the name, an extension for a Lua file or a hashed
    // name), each name written once, as rawfiles/<name>, the name's folders kept.
    using RawfileReader = std::function<bool(int64_t Pos, std::string& Name, std::vector<uint8_t>& Bytes)>;
    void ExportRawfiles(const DiskGame::Context& C, DiskGame::Totals& T, const std::vector<std::pair<const char*, RawfileReader>>& Readers);
    // A rawfile's bytes in the shape most games keep: Size bytes (u32 at SizeAt) at the pointer at
    // DataAt, or, when PackedAt (-1: none) holds a non-zero u32, that many zlib bytes inflating to
    // Size (Size + 1 when the game counts the NUL: what inflates is cut to Size)
    struct RawLayout
    {
        int64_t SizeAt = 0, DataAt = 0, PackedAt = -1;
    };
    bool RawBytes(int64_t Pos, const RawLayout& L, std::vector<uint8_t>& Out) const;
    // The rawfile kind of Modern Warfare III, Black Ops 6 and 7: rawfile (0x20) {hash, u32 extension
    // hash 8 (FNV-1a 32 of ".json"...), compressed size 0xc, size 0x10, buffer 0x18}, a name no
    // index knows rawfile_<hash> with its extension; luafile {hash, i32 size at LuaSize, buffer at
    // LuaData}
    void ExportsHashedRawfiles(int64_t LuaSize, int64_t LuaData);
    // The material links kind (Exports(Kind::MaterialLinks, ...)), for Modern Warfare III, Black Ops
    // 6 and 7: a material's texture slots {u8 semantic, u8 image index} (u8 count at SlotCount) at
    // Slots and its image pointers (u8 count at ImageCount) at Images, an image GfxImage (0x40)
    // {hash 0, width u16 0x20, height u16 0x22, format u8 0x2a}; with Slots -1 (Modern Warfare
    // III's campaign) Images holds SlotCount records {u32 semantic, image pointer at 8}. One CSV
    // per zone, a row per slot:
    // material_id,slot,semantic,image_id,image,width,height,format (ids on 63 bits; image: "zone"
    // its data is here, "named" the zone holds its hash alone, "hash" the pointer is the hash with
    // bit 63 set, "beside" in a zone served with this one); the engine's default images left out
    // With Techset (the techset pointer's offset), materials/<zone>.csv beside it, a row per
    // material: material_id,techset_id,flags,layers,constants,constants_id (flags: the u32 at
    // Flags; layers: the u8 at Layers, how many layers a layered material blends, 0 for any
    // other; constants: the 16-byte constants at Constants, u8 count at ConstantCount, and the
    // FNV-1a 64 of their bytes). The techset decides the prefix of the material's name (m, tm,
    // twc...). With ModelMaterials (an xmodel's material handles: name 8, u16 count 0x10),
    // models/<zone>.csv: model_id,model,surface,material_id, a model's materials in surface order
    struct MaterialLinkLayout
    {
        int64_t SlotCount = 0, ImageCount = 0, Slots = 0, Images = 0;
        int64_t Techset = -1, Flags = 0, Layers = 0, Constants = 0, ConstantCount = 0, ModelMaterials = -1;
        // the records of Slots -1, and the image's fields: Black Ops 4's records are 0x20 {image 0,
        // semantic hash u32 8}, its image {hash 0x20, width u16 0x68, height 0x6a, format u32 0x64}
        int64_t RecordSize = 16, RecordSemantic = 0, RecordImage = 8;
        int64_t ImageHash = 0, ImageWidth = 0x20, ImageHeight = 0x22, ImageFormat = 0x2a;
        bool WideFormat = false;   // the format a u32, else a u8
    };
    void ExportsMaterialLinks(const MaterialLinkLayout& L);
    // The sound links kind (Exports(Kind::SoundLinks, ...)), for Modern Warfare III, Black Ops 6
    // and 7: a soundbank (or soundbanktransient) holds its aliases at Aliases (u32 count at
    // AliasCount), AliasSize bytes each {hash at AliasHash, entries at Entries, u16 count at
    // EntryCount}, an entry EntrySize bytes with its sndasset at EntrySound (in this zone, or, bit
    // 63, its hash alone). One TSV per zone, a row per entry: alias_id<TAB>file_id<TAB>index (ids
    // on 63 bits in hex, index: the entry's rank in its alias)
    struct SoundLinkLayout
    {
        int64_t Aliases = 0, AliasCount = 0, AliasSize = 0, AliasHash = 0, Entries = 0, EntryCount = 0, EntrySize = 0, EntrySound = 0;
        // Black Ops 4: the bank is the "sound" asset (SndBank) and an entry holds its file's hash
        // itself (SndAlias assetId), not a pointer
        const char* Bank = nullptr;   // nullptr: soundbank and soundbanktransient
        bool SoundIsHash = false;
    };
    void ExportsSoundLinks(const SoundLinkLayout& L);
    // The asset references kind (Exports(Kind::AssetRefs, ...)): what each zone-level asset (but
    // the bulk ones: images, materials, models, meshes, anims, sounds, shaders, the map's world)
    // names. One CSV per zone, asset_type,asset_id,what,value: "hash" a 64-bit value of its data
    // that is an asset's hash (bit 63 set) or could be one (not a pointer, 2^40 or more), "ptr" the
    // id of a zone-level asset of this zone it points to (with its type), "str" a string it holds
    // (through Decrypt when the game encrypts them). Ids on 63 bits (an asset that keeps no hash
    // in front: @ and its position in the zone); values are not checked: a join on known ids
    // sorts them out
    void ExportsAssetRefs(std::function<std::string(const std::string&)> Decrypt = nullptr);
    // The techsets kind (Exports(Kind::Techsets, ...), Techsets.h), for Modern Warfare III, Black
    // Ops 6 and 7: the zone's techsets, their shaders (bytecode from the zone or the install's
    // packages) and its materials' bindings, read by the game's Format
    void ExportsTechsets(std::shared_ptr<const Techsets::Format> F);
    // The script bundles kind (Exports(Kind::ScriptBundles, ...), ZonekitZone.Bundles.cpp), for Black
    // Ops 6 and 7: a scriptbundle {hash 0, settings type hash, key/value table}; a table {keys'
    // FNV-1a 64 (sorted), script identifier hashes (var_<hash>), entries {u8 type, value at 8}, u32
    // count}; Types maps a type code to its value. One JSON per bundle, bundles/<zone>/<name>.json
    struct ScriptBundleLayout
    {
        int64_t TypeHash = 8, Table = 0x10;
        int64_t Keys = 0, ScriptHashes = 8, Entries = 0x10, Count = 0x18;
        // an array: Black Ops 7 {u8 element type, 8-byte elements at 8, u32 count 0x18}; Black Ops 6
        // {16-byte entries typed one by one at 0, u32 count 0x10} (ArrayType -1, ArrayStride 0x10)
        int64_t ArrayType = 0, ArrayElements = 8, ArrayCount = 0x18, ArrayStride = 8;
        // by type code, Black Ops 6 and 7's loader (BO7 0x3888060): 6 and 7 a table, 8 an array,
        // 9 and 18 a record, 11 and 17 a hash, 4 a localized string's hash, 15 an asset's; 10,
        // 12-14, 16 written raw
        std::vector<BundleValue> Types = { BundleValue::Raw,    BundleValue::Bool,   BundleValue::Int,         BundleValue::Float,
                                           BundleValue::Localize, BundleValue::String, BundleValue::Struct,     BundleValue::KeyedStruct,
                                           BundleValue::Array,  BundleValue::Anim,   BundleValue::Raw,         BundleValue::Hash,
                                           BundleValue::Raw,    BundleValue::Raw,    BundleValue::Raw,         BundleValue::Asset,
                                           BundleValue::Raw,    BundleValue::Hash,   BundleValue::Anim };
        // the settings type's name hash, which proves a name found for it ("type_name"): Black Ops 6
        // and 7's FNV-1a 64 of the lowercase name, 63 bits (itemspawnlist 729e042db93842a1)
        std::function<uint64_t(const std::string&)> TypeNameHash = [](const std::string& S) {
            uint64_t H = 0xCBF29CE484222325ull;
            for (const char C : S) H = (H ^ (uint8_t)(C >= 'A' && C <= 'Z' ? C + 0x20 : C)) * 0x100000001B3ull;
            return H & 0x7FFFFFFFFFFFFFFFull;
        };
    };
    void ExportsScriptBundles(const ScriptBundleLayout& L, std::function<std::string(const std::string&)> Decrypt = nullptr);
    // Another asset type holding a script bundle's key/value table at Table (Black Ops 7's mapinfo
    // at 0xc8), written by K the same way: <kind>/<zone>/<name>.json, plus its string fields (by
    // name, from Strings) and hash fields (Hashes); its name, when no index knows its hash, the one
    // of its strings or of the zone's name whose NameHash equals it
    struct BundleOwner
    {
        std::string Type;
        int64_t Table = 0x10, TypeHash = -1;
        std::vector<std::pair<std::string, int64_t>> Strings, Hashes;
        std::function<uint64_t(const std::string&)> NameHash;
    };
    void ExportsBundleTrees(DiskGame::Kind K, std::vector<BundleOwner> Owners, const ScriptBundleLayout& L,
                            std::function<std::string(const std::string&)> Decrypt = nullptr);
    // A struct dumps kind (Exports(K, ...), ZonekitZone.Dump.cpp), for assets whose fields nobody
    // has named yet: each asset of Types (zone-level or inlined in another), once per type and
    // hash, as JSON: its struct by offset, each 4-byte word read as a plausible float or an
    // integer, a pointer field followed: a string, another asset (its type and name), or a block
    // the walk read (its size: the largest read starting there), dumped the same way; a pointer
    // left unlinked by the walk (an asset of another zone) as its hash and the type its chain
    // reads. Keys are offsets: two assets of a type diff field by field. An asset is named by its
    // hash, else by the string its NameAt field points to, else after the one asset of Types
    // pointing to it (#n: the n-th of its type there). <kind>/<zone>/<type>/<name>.json
    // A field named once proven: shown as "<offset> <name>" (the offset kept, so a dump still diffs
    // by offset), its value read as Kind says ("i32", "f32", "u16x2" two u16 as [low, high]; "" the
    // dump's own guess; "hash" an 8-byte hash, {"id", "name" when known}, or 0); public: a game
    // family's helpers name them
public:
    struct DumpField
    {
        std::string Name, Kind;
    };

protected:
    struct DumpType
    {
        std::string Type;
        int64_t NameAt = -1;   // the field pointing to its name, -1: none
        // its named fields by path from the asset ("0x0020/0x1afb": a field of the block at 0x20)
        std::map<std::string, DumpField> Fields;
        int64_t IdAt = 0;   // the field holding its 63-bit name hash
    };
    // NamesFrom: a gdt type of the profile (<profile>/gdt/<type>/, Gdt::Type::NamedFields) whose layouts
    // name more fields of these types: the names the game's own field list gives ("source": "engine"),
    // a 4-byte number as its form says (an array's elements name[i]), a pointer or hash as the dump
    // guesses; bytes and the "observed" names are left out, a field the game names itself kept
    void ExportsStructDumps(DiskGame::Kind K, std::vector<DumpType> Types, std::function<std::string(const std::string&)> Decrypt = nullptr,
                            std::string NamesFrom = std::string());
    // A kind written as one JSON per zone-level asset of Type (Black Ops 4 and Cold War's script
    // bundles): Read gives its text and the folder it goes in under its zone ("" none; false: not
    // read, a failure; true with no text: nothing to write, a name-only header). Reads: the asset's
    // reads (position, size) in walk order, where a block the zone keeps inline is. Named by the hash
    // at its offset 0 (HashName, else <Type>_<hex>): <kind>/<zone>/[<folder>/]<name>.json, indexed
    using JsonAssetReader =
        std::function<bool(int64_t Pos, const std::vector<std::pair<int64_t, int64_t>>& Reads, std::string& Folder, std::string& Json)>;
    void ExportJsonAssets(DiskGame::Kind K, const DiskGame::Context& C, DiskGame::Totals& T, const char* Type, const JsonAssetReader& Read);
    // A Lua file's name ending as its bytes are: .luac for compiled bytecode (ESC "Lua", LuaJIT's
    // ESC "LJ"), else .lua (in place of the name's own .lua or .luac); deflated bytecode (zlib)
    // inflated first
    static void NameLua(std::string& Name, std::vector<uint8_t>& Bytes);

    // The data kind's string tables (ZonekitZone.Tables.cpp, Exports(Kind::Data, ...)): each
    // top-level asset of Type, named by AssetName(Kind::Data) (a reference left out), its cells
    // read by Read (Columns x Rows, row by row; false: not read, a failure) and written as CSV by
    // ExportWriters::WriteStringTable.
    using StringTableReader = std::function<bool(int64_t Pos, uint32_t& Columns, uint32_t& Rows, std::vector<std::string>& Cells)>;
    void ExportStringTables(const DiskGame::Context& C, DiskGame::Totals& T, const char* Type, const StringTableReader& Read);
    // The classic string table: i32 column and row counts at ColumnsAt and RowsAt, the cells at the
    // pointer at CellsAt, Cell bytes each, a string pointer first (CoD4 to WWII, CODOL)
    struct StringTableLayout
    {
        int64_t ColumnsAt = 8, RowsAt = 0xc, CellsAt = 0x10, Cell = 0x10;
    };
    bool StringTableCells(int64_t Pos, const StringTableLayout& L, uint32_t& Columns, uint32_t& Rows, std::vector<std::string>& Cells) const;
    // A hash a table cell or a hashed name holds, by name: HashName's, else the package index's
    // asset and string names; "" when none knows it
    std::string KnownHash(uint64_t Hash) const;
    // The column string table of Modern Warfare III, Black Ops 6 and 7 (0x28): {hash, i32 columns 8,
    // rows 0xc, columns 0x18 of 0x28 bytes {type u8 0, u16 values 2, u16 rows 4, a u16 value index
    // per row 0x10, the values 0x20}}, each value written as acts writes it: a string as it is
    // (through Text: a cipher), an int, a float, TRUE/FALSE, a hash as its prefix and name (#, %#,
    // @#, x32#; Black Ops 6 and 7 also o#, s#, &#; MWIII's localized strings & and a&)
    enum class ColumnTypes { Mwiii, Bo6 };
    bool ColumnTableCells(int64_t Pos, ColumnTypes Types, const std::function<std::string(const std::string&)>& Text, uint32_t& Columns,
                          uint32_t& Rows, std::vector<std::string>& Cells) const;
    // The data kind's string tables of Modern Warfare III, Black Ops 6 and 7, by ColumnTableCells
    void ExportsColumnTables(ColumnTypes Types, std::function<std::string(const std::string&)> Text);

    // A hash field's name ("" when the index does not know it) and the bits a hash field holds
    // (the gdt kind); a game whose fields hold hashes says both.
    virtual std::string HashName(uint64_t Hash) const { return std::string(); }
    virtual uint64_t HashBits() const { return ~0ull; }
    // The asset of a type at Pos by name: a model or an animation as their exporters name them,
    // any other by the hash at its first field (hash_<hex> when not known)
    std::string NamedAsset(const std::string& Type, int64_t Pos) const;

    // A ScriptReader for an IW scriptfile, 64-bit: name 0, compressedLen 8, len 0xc, bytecodeLen
    // 0x10, the zlib stack 0x18, the bytecode 0x20; read as gsc-tool's .gscbin
    bool ReadScriptFile(int64_t Pos, Scripts::Script& Out) const;
    // The scripts kind of a game whose scripts are its scriptfile assets (ReadScriptFile)
    void ExportsScriptFiles(const Scripts::Vm& Vm);

    template <class T>
    static T At(ByteView Buf, int64_t Off) { return ExportNaming::At<T>(Buf, Off); }

    // CoDAssets' package cache, which the live readers read pixels and meshes through, set to the
    // one Make gives while the zone is served (On), the previous one back when it is not
    void ServePackages(bool On, const std::function<std::shared_ptr<CoDPackageCache>()>& Make);
    // A package cache that reads through one every zone of the run shares (its objects only:
    // Exists and the file system answer as an empty cache's)
    static std::shared_ptr<CoDPackageCache> SharedPackages(std::shared_ptr<CoDPackageCache> Packages);

    // The zone's script strings by slot, which SlotString names bones and notetracks with: the
    // game fills them when the zone opens (Fixups, or Handlers when they depend on the bytes only)
    std::vector<std::string> Slots;
    // Count script string slots of SlotSize bytes (4 or 8) from First: a slot equal to Marker holds
    // a string whose text follows the slots, in slot order; the others hold none ("")
    static std::vector<std::string> InlineStrings(ByteView Zone, int64_t First, uint64_t Count, int SlotSize,
                                                  uint64_t Marker);
    // A string handler for Handlers(): the served zone's slot, else bone_<n>
    static std::string SlotString(uint64_t Index);
    // The session's handlers are plain functions: they reach the zone served now (opened or
    // resumed last, not parked since) through this
    template <class Z>
    static Z* ServedZone() { return static_cast<Z*>(Serving); }
    bool IsServed() const { return Serving == this; }
    // The textures the models (or an effect's materials) named, written by this zone's image
    // exporter (Only those names); what it does not hold is needed from the zone that defines it.
    void ExportTextures(const std::set<std::string>& Names, const DiskGame::Context& C, DiskGame::Totals& T);
    // the kind's assets of the type, each by the live reader (a game wrapping its model kind calls it)
    void ExportLive(DiskGame::Kind K, const char* Type, const DiskGame::Context& C, DiskGame::Totals& T);

private:
    static ZonekitZone* Serving;

    // the session just began (opened or resumed, its lock held): the zone becomes Serving, its
    // held packages CoDAssets' package cache until it is parked or destroyed
    void BeginServing();
    void Relocate(const ResolvedLinks& L);
    void ExportLocalize(const DiskGame::Context& C, DiskGame::Totals& T);
    void ExportGdt(const DiskGame::Context& C, DiskGame::Totals& T);
    // the profile's gdt types (<profile>/gdt/<type>/), read once per profile; empty and Error when they do not read
    std::vector<std::shared_ptr<const Gdt::Type>> GdtTypes(std::string& Error) const;
    // how the gdt layouts read this zone (its image, names, strings)
    Gdt::Memory GdtMemory() const;

    std::string Path, ZoneName, ProfileName;
    ZoneBytes Bytes;
    ZoneTables::ZoneResult R;
    mutable std::unique_ptr<ZoneTables::BlockMap> Memory;   // R's stream layout, built by TextAt
    const ExportWriters::LoadedProfile* Loaded = nullptr;
    std::shared_ptr<CoDPackageCache> Held;   // ServesPackages'
    std::unique_ptr<VirtualGameSession> Session;
    ZoneMemory::Copy ParkedImage;   // the session's relocated zone while parked
    // Serving set to this zone while served, CoDAssets' package cache while ServePackages serves:
    // under the session's lock (after Session: they end first, an exception's unwinding too)
    std::optional<CoDAssets::Scoped<ZonekitZone*>> ServingThis;
    std::optional<CoDAssets::Scoped<std::shared_ptr<CoDPackageCache>>> ServedPackages;
    bool Walked = false;
    bool Partial = false;             // opened from the resume index, only some assets walked
    std::set<std::string> Listed;     // a partial open's: the types its asset list names
    mutable ByteView ZoneView, ImageView;   // Zone() and Image()
    struct
    {
        std::vector<std::pair<uint64_t, int64_t>> Keys;
        std::map<std::pair<std::string, uint64_t>, int64_t> ByHash;
    } Whole;                          // a partial open's: its resume index's keys and hashes
    std::map<std::string, std::vector<int64_t>> WholeRanked;   // a partial open's: Ranked's instances
    const char* MaterialType = nullptr;   // ExportsMaterials' type: Defines lists those materials
    std::function<XMaterial_t(uint64_t Address)> MaterialOf;   // ReadsMaterials'
    // the model kind's: the images of the material headers this zone names (MaterialImages, its
    // own or Context::MaterialsElsewhere's), for ModelTextures::Fill; empty when none is
    std::function<void(XMaterial_t& Material)> HeaderFiller(const DiskGame::Context& C);
    std::map<std::string, size_t> Counters;   // Measures() adds them to the run's totals
    std::map<std::string, size_t> SoundFileCounts;   // WriteSoundFile's with a sink, under its lock
    std::vector<std::pair<std::string, double>> Steps;   // Open's, in seconds
};
