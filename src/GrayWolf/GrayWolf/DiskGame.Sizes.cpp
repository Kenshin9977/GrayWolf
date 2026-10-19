#include "stdafx.h"

// DiskGame::Sizes (DiskGameSizes.h): the export size estimate's samples and its arithmetic.

#include "DiskGameSizes.h"

#include "ExportNaming.h"
#include "SettingsManager.h"
#include "Strings.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <windows.h>

namespace fs = std::filesystem;

namespace DiskGame::Sizes
{
namespace
{
    // A format setting, the files it writes (by extension) and its size relative to the family's
    // first format: what converts a sample written in one format into another (an approximation,
    // said so). The ratios are averages measured on samples of Black Ops II, Black Ops III and
    // Ghosts zones written in every format.
    struct Format
    {
        const char* Key;   // its setting ("export_castmdl"), or the value of the family's setting ("PNG")
        std::vector<const char*> Extensions;
        double Ratio;
        const char* Label;
    };
    const std::vector<Format> ModelFormats = {
        { "export_castmdl", { ".cast" }, 1.0, "Cast" },
        { "export_semodel", { ".semodel" }, 1.06, "SEModel" },
        { "export_obj", { ".obj", ".mtl" }, 6.35, "OBJ" },
        { "export_ma", { ".ma", ".mel" }, 7.15, "Maya" },
        { "export_xna", { ".ascii" }, 3.0, "XNALara" },
        { "export_xmexport", { ".xmodel_export" }, 8.7, "XMODEL_EXPORT" },
        { "export_xmbin", { ".xmodel_bin" }, 0.76, "XMODEL_BIN" },
        { "export_smd", { ".smd" }, 6.6, "SMD" },
        { "export_gltf", { ".gltf", ".bin" }, 1.37, "glTF" },
        { "export_glb", { ".glb" }, 1.33, "GLB" },
    };
    // XANIM_EXPORT files have no extension
    const std::vector<Format> AnimFormats = {
        { "export_castanim", { ".cast" }, 1.0, "Cast" },
        { "export_seanim", { ".seanim" }, 0.82, "SEAnim" },
        { "export_directxanim", { "" }, 0.37, "XANIM_EXPORT" },
    };
    // exportimg's values; DDS keeps the game's block compression (and mips)
    const std::vector<Format> ImageFormats = {
        { "DDS", { ".dds" }, 1.0, "DDS" },
        { "PNG", { ".png" }, 1.75, "PNG" },
        { "TGA", { ".tga" }, 5.0, "TGA" },
        { "TIFF", { ".tiff", ".tif" }, 1.75, "TIFF" },   // written as the PNG is today
    };
    // exportsnd's values
    const std::vector<Format> SoundFormats = {
        { "WAV", { ".wav" }, 1.0, "WAV" },
        { "FLAC", { ".flac" }, 0.5, "FLAC" },   // 0.32 to 0.65 by game
        { "OPUS", { ".opus", ".ogg" }, 0.086, "Opus" },
    };
    // WriteSound's FLAC encoder over 16-bit PCM (PCM or ADPCM sources decoded): 0.53 to 0.60 on Black Ops,
    // Black Ops II, World at War and Advanced Warfare sounds
    constexpr double FlacOfPcm = 0.57;
    // exportalllods on: every LOD written, not the first alone
    constexpr double AllLods = 1.7;

    // A level's export (its .glb and import script, its textures) per byte of its zone's footprint
    // when no level was sampled: 0.66 to 3 on Black Ops II, III and Ghosts levels; and what each
    // map setting multiplies it by
    constexpr double MapPerFootprint = 1.0;
    // (Black Ops III's mp_freerun_04: the resized and PNG copies added nothing there, its images
    // being PNG already and none over 2k; Radiant writes the world and models as text)
    constexpr double MapTerrain = 0.001, MapTextures2k = 1.0, MapTextures1k = 1.0, MapPng = 1.0, MapCast = 1.08, MapRadiant = 2.7,
                     MapCollision = 1.04;

    const char* ImageKey = "exportimg";
    const char* SoundKey = "exportsnd";

    std::map<std::string, std::string> Parse(const std::string& Formats)
    {
        std::map<std::string, std::string> Out;
        std::istringstream In(Formats);
        for (std::string Item; std::getline(In, Item, ';');)
            if (const size_t Eq = Item.find('='); Eq != std::string::npos) Out[Item.substr(0, Eq)] = Item.substr(Eq + 1);
        return Out;
    }

    std::string Of(const std::map<std::string, std::string>& M, const std::string& Key)
    {
        const auto It = M.find(Key);
        return It == M.end() ? std::string() : It->second;
    }

    // What the map settings make a level write, relative to the defaults (the textures as they
    // are, the .glb alone), as measured on a Black Ops III level
    double MapSettings(const std::map<std::string, std::string>& S)
    {
        double F = 1.0;
        if (Of(S, "map_content") == "terrain") F *= MapTerrain;
        const std::string Tex = Of(S, "map_textures");
        if (Tex == "2k") F *= MapTextures2k;
        else if (Tex == "1k") F *= MapTextures1k;
        if (Of(S, "map_png") == "on") F *= MapPng;
        if (Of(S, "map_cast") == "on") F *= MapCast;
        if (Of(S, "map_radiant") == "on") F *= MapRadiant;
        if (Of(S, "map_collision") == "on") F *= MapCollision;
        return F;
    }

    const Format* FormatOf(const std::vector<Format>& Family, const std::string& Key)
    {
        for (const auto& F : Family)
            if (_stricmp(F.Key, Key.c_str()) == 0) return &F;
        return nullptr;
    }

    bool Owns(const std::vector<Format>& Family, const std::string& Extension)
    {
        for (const auto& F : Family)
            for (const char* E : F.Extensions)
                if (Extension == E) return true;
        return false;
    }

    uint64_t BytesOf(const Sample& S, const Format& F)
    {
        uint64_t N = 0;
        for (const char* E : F.Extensions)
            if (const auto It = S.Bytes.find(E); It != S.Bytes.end()) N += It->second;
        return N;
    }

    uint64_t AllBytes(const Sample& S) { return Written(S); }

    // What a sample of a family with several formats ticked at once (models, anims) writes in the
    // formats ticked now: the files of no format (a material sidecar) always; a format measured as
    // it is; one that was not, from the measured ones by its ratio (Approx). Why says which.
    double FormatsBytes(const Sample& S, const std::vector<Format>& Family, const std::map<std::string, std::string>& Then,
                        const std::map<std::string, std::string>& Now, bool& Approx, bool& Unknown, std::string& Why)
    {
        double Bytes = 0;
        for (const auto& [E, B] : S.Bytes)
            if (!Owns(Family, E)) Bytes += (double)B;
        // a measured format to convert from: the one that wrote most
        const Format* From = nullptr;
        uint64_t FromBytes = 0;
        for (const auto& F : Family)
            if (Of(Then, F.Key) == "true" && BytesOf(S, F) > FromBytes) From = &F, FromBytes = BytesOf(S, F);
        std::string Measured, Converted;
        for (const auto& F : Family)
        {
            if (Of(Now, F.Key) != "true") continue;
            if (Of(Then, F.Key) == "true")
            {
                Bytes += (double)BytesOf(S, F);
                Measured += std::string(Measured.empty() ? "" : ", ") + F.Label;
            }
            else if (From != nullptr)
            {
                Bytes += (double)FromBytes * F.Ratio / From->Ratio;
                Converted += std::string(Converted.empty() ? "" : ", ") + F.Label;
                Approx = true;
            }
            else
                Unknown = true;
        }
        if (!Measured.empty()) Why += "measured as " + Measured;
        if (!Converted.empty()) Why += std::string(Why.empty() ? "" : "; ") + Converted + " converted from " + From->Label + " by a typical ratio";
        return Bytes;
    }

    // A one-format family (images, sounds): the sample's bytes, converted when the format changed
    double FormatBytes(double Bytes, const std::vector<Format>& Family, const std::string& Then, const std::string& Now, bool& Approx,
                       std::string& Why)
    {
        const Format* A = FormatOf(Family, Then);
        const Format* B = FormatOf(Family, Now);
        if (A == nullptr || B == nullptr || A == B)
        {
            Why += std::string("measured as ") + (A ? A->Label : Then.c_str());
            return Bytes;
        }
        Approx = true;
        Why += std::string("measured as ") + A->Label + ", converted to " + B->Label + " by a typical ratio";
        return Bytes * B->Ratio / A->Ratio;
    }

    using ExportNaming::Thousands;

    // Every file under Dir: bytes by extension (a hard link counted once over its links), and Each
    // told of each file (its path, its bytes)
    void Tally(const fs::path& Dir, std::map<std::string, uint64_t>& Out,
               const std::function<void(const fs::path&, uint64_t)>& Each = nullptr)
    {
        std::error_code Ec;
        if (!fs::is_directory(Dir, Ec)) return;
        for (auto It = fs::recursive_directory_iterator(Dir, fs::directory_options::skip_permission_denied, Ec);
             !Ec && It != fs::recursive_directory_iterator(); It.increment(Ec))
        {
            std::error_code E2;
            if (!It->is_regular_file(E2)) continue;
            const uint64_t Size = It->file_size(E2);
            const uint64_t Links = std::max<uint64_t>(1, fs::hard_link_count(It->path(), E2));
            Out[ExportNaming::Lower(It->path().extension().string())] += Size / Links;
            if (Each) Each(It->path(), Size / Links);
        }
    }
}

SoundRow RowOf(const SoundHeader& H)
{
    SoundRow R;
    R.Key = ExportNaming::Fnv1a(ExportNaming::Lower(H.Name));
    R.Is = H.Is, R.KeptUnderWav = H.KeptUnderWav;
    R.Rate = H.Rate, R.Channels = H.Channels, R.Stored = H.Stored, R.Frames = H.Frames, R.Bits = H.Bits ? H.Bits : 16;
    return R;
}

bool FlacStreamInfo(const uint8_t* Data, size_t Size, uint32_t& Rate, uint32_t& Channels, uint32_t& Bits, uint64_t& Frames)
{
    // "fLaC", a metadata block header (type 0: STREAMINFO), then its 34 bytes: block and frame
    // sizes (10), then rate 20 bits, channels - 1 3 bits, bits - 1 5 bits, samples 36 bits
    if (Size < 26 || std::memcmp(Data, "fLaC", 4) != 0 || (Data[4] & 0x7f) != 0) return false;
    const uint8_t* P = Data + 18;
    uint64_t V = 0;
    for (int I = 0; I < 8; I++) V = (V << 8) | P[I];
    Rate = (uint32_t)(V >> 44);
    Channels = (uint32_t)((V >> 41) & 7) + 1;
    Bits = (uint32_t)((V >> 36) & 31) + 1;
    Frames = V & 0xFFFFFFFFFull;
    return Rate != 0;
}

SoundHeader HeaderOf(const ExportWriters::SoundSource& S, const std::string& Name)
{
    using Form = ExportWriters::SoundSource::Form;
    SoundHeader H;
    H.Name = Name;
    H.Stored = S.Size;
    H.Rate = S.Rate, H.Channels = S.Channels, H.Frames = S.Frames;
    H.KeptUnderWav = S.KeptUnderWav;
    if (S.Is == Form::Pcm16)
    {
        H.Is = SoundHeader::Form::Pcm16;
        H.Frames = S.Channels ? S.Size / (2ull * S.Channels) : 0;
        return H;
    }
    if (S.Is == Form::Opus)
    {
        H.Is = SoundHeader::Form::Opus;
        return H;
    }
    const std::string Ext = ExportNaming::Lower(S.Extension);
    H.Frames = 0;
    if (Ext == ".flac")
    {
        H.Is = SoundHeader::Form::Flac;
        FlacStreamInfo(S.Data, S.Size, H.Rate, H.Channels, H.Bits, H.Frames);
        return H;
    }
    // a .wav file (or another, written as it is under WAV): its fmt and data chunks, or a FLAC
    // stream behind its header (WWII)
    H.Is = SoundHeader::Form::Wav;
    if (Ext != ".wav" || S.Size < 12 || std::memcmp(S.Data, "RIFF", 4) != 0) return H;
    uint16_t Tag = 0, Bits = 0, Channels = 0, BlockAlign = 0, PerBlock = 0;
    uint32_t Rate = 0, Fact = 0;
    for (size_t At = 12; At + 8 <= S.Size;)
    {
        uint32_t Len = 0;
        std::memcpy(&Len, S.Data + At + 4, 4);
        const uint8_t* Body = S.Data + At + 8;
        if (std::memcmp(S.Data + At, "fmt ", 4) == 0 && Len >= 16 && At + 8 + 16 <= S.Size)
        {
            std::memcpy(&Tag, Body, 2), std::memcpy(&Channels, Body + 2, 2), std::memcpy(&Rate, Body + 4, 4), std::memcpy(&Bits, Body + 14, 2);
            std::memcpy(&BlockAlign, Body + 12, 2);
            // IMA ADPCM's samples per block, after its extra size
            if (Len >= 20 && At + 8 + 20 <= S.Size) std::memcpy(&PerBlock, Body + 18, 2);
        }
        else if (std::memcmp(S.Data + At, "fact", 4) == 0 && Len >= 4 && At + 12 <= S.Size)
            std::memcpy(&Fact, Body, 4);   // a compressed WAV's sample count
        else if (std::memcmp(S.Data + At, "data", 4) == 0)
        {
            const uint64_t Data = std::min<uint64_t>(Len, S.Size - (At + 8));
            if (Data >= 4 && std::memcmp(Body, "fLaC", 4) == 0) FlacStreamInfo(Body, (size_t)Data, H.Rate, H.Channels, H.Bits, H.Frames);
            else if (Tag == 1 && Channels && Bits >= 8)
            {
                H.Rate = Rate, H.Channels = Channels, H.Bits = Bits;
                H.Frames = Data / ((uint64_t)Channels * (Bits / 8));
            }
            else if (Channels && (Fact || (BlockAlign && PerBlock)))
            {
                // ADPCM (World at War, Black Ops): its length from its fact chunk or its blocks,
                // decoded to 16-bit PCM
                H.Rate = Rate, H.Channels = Channels, H.Bits = 16;
                H.Frames = Fact ? Fact : Data / BlockAlign * PerBlock;
            }
            break;
        }
        At += 8 + Len + (Len & 1);
    }
    return H;
}

uint64_t SoundBytes(const SoundRow& R, const std::string& Format, bool& Exact)
{
    using Form = SoundHeader::Form;
    constexpr uint64_t WavHeader = 44;
    // its samples as PCM (a FLAC decodes to its own bit depth), when the header gives its length
    const uint64_t Pcm = R.Frames * R.Channels * (R.Bits / 8);
    const std::string F = Strings::ToUpper(Format);
    Exact = true;
    if (R.Is == Form::Other) return R.Stored;   // kept as it is, whatever the format
    if (F == "FLAC")
    {
        if (R.Is == Form::Flac) return R.Stored;
        Exact = false;   // encoded
        return (uint64_t)((double)(Pcm ? Pcm : R.Stored) * FlacOfPcm);
    }
    if (F == "OPUS")
    {
        Exact = false;   // remuxed into Ogg, or encoded
        if (R.Is == Form::Opus) return R.Stored + R.Stored / 50;
        // WriteSound's encoder: 64 kb/s a channel over the sound's length
        if (R.Frames && R.Rate && R.Channels) return (uint64_t)((double)R.Frames / R.Rate * 8000.0 * R.Channels);
        const double Wav = Pcm ? (double)Pcm : R.Is == Form::Flac ? (double)R.Stored / SoundFormats[1].Ratio : (double)R.Stored;
        return (uint64_t)(Wav * SoundFormats[2].Ratio);
    }
    // WAV: a file as the game keeps it, a FLAC decoded to 16-bit PCM, samples behind a header
    if (R.Is == Form::Wav || (R.Is == Form::Flac && R.KeptUnderWav)) return R.Stored;
    if (R.Is == Form::Pcm16) return WavHeader + R.Stored;
    if (Pcm) return WavHeader + Pcm;
    Exact = false;
    return R.Is == Form::Flac ? (uint64_t)((double)R.Stored / SoundFormats[1].Ratio) : R.Stored;
}

uint64_t Written(const Sample& S)
{
    uint64_t N = 0;
    for (const auto& [E, B] : S.Bytes) N += B;
    return N;
}

std::string FormatsNow()
{
    std::string S = std::string(ImageKey) + "=" + Strings::ToUpper(SettingsManager::GetSetting(ImageKey, "PNG")) + ";" + SoundKey + "=" +
                    Strings::ToUpper(SettingsManager::GetSetting(SoundKey, "WAV"));
    for (const auto* Family : { &ModelFormats, &AnimFormats })
        for (const auto& F : *Family) S += std::string(";") + F.Key + "=" + SettingsManager::GetSetting(F.Key, "false");
    for (const char* K : { "exportalllods", "exportmodelimg" }) S += std::string(";") + K + "=" + SettingsManager::GetSetting(K, "false");
    return S;
}

bool Load(const std::string& File, Calibration& Out)
{
    std::ifstream In(File, std::ios::binary);
    std::string Line;
    if (!In || !std::getline(In, Line) || Line != "greyhound-sizes 3") return false;
    Calibration C;
    Sample* At = nullptr;
    std::vector<SoundRow>* Rows = nullptr;
    while (std::getline(In, Line))
    {
        std::istringstream L(Line);
        std::string Head;
        L >> Head;
        if (Head == "formats") L >> C.Formats;
        else if (Head == "settings") L >> C.Settings;
        else if (Head == "soundheaders") C.SoundHeaders = true;
        else if (Head == "zonesounds")
        {
            // zonesounds <footprint> <zone>, then an "s" line per sound
            uint64_t Footprint = 0;
            std::string Zone;
            L >> Footprint;
            std::getline(L >> std::ws, Zone);
            Rows = &C.Sounds[Zone];
            C.SoundFootprints[Zone] = Footprint;
        }
        else if (Head == "s" && Rows != nullptr)
        {
            SoundRow R;
            int Is = 0, Kept = 0;
            if (L >> std::hex >> R.Key >> std::dec >> Is >> Kept >> R.Rate >> R.Channels >> R.Stored >> R.Frames >> R.Bits)
            {
                R.Is = (SoundHeader::Form)Is;
                R.KeptUnderWav = Kept != 0;
                Rows->push_back(R);
            }
        }
        else if (Head == "kind")
        {
            std::string Name;
            L >> Name;
            At = nullptr;
            for (const KindInfo& I : Kinds)
                if (Name == I.Name) At = &C.Kinds[I.Id];
            if (At == nullptr) continue;
            int Listed = 0, Cut = 0;
            L >> Listed >> At->Assets >> At->Textures >> At->ListedTextures >> At->ZoneFootprint >> At->ZoneBytes >> At->ZoneAssets >> Cut >> At->Seconds;
            At->Listed = Listed != 0;
            At->Cut = Cut != 0;
            std::getline(L >> std::ws, At->Zone);
        }
        else if (Head == "bytes" && At != nullptr)
        {
            std::string Extension;
            uint64_t N = 0;
            if (L >> Extension >> N) At->Bytes[Extension == "-" ? std::string() : Extension] = N;
        }
    }
    Out = std::move(C);
    return true;
}

void Save(const std::string& File, const Calibration& C)
{
    std::ofstream Out(File, std::ios::binary | std::ios::trunc);
    Out << "greyhound-sizes 3\nformats " << C.Formats << "\nsettings " << C.Settings << "\n";
    for (const auto& [K, S] : C.Kinds)
    {
        Out << "kind " << KindName(K) << " " << (S.Listed ? 1 : 0) << " " << S.Assets << " " << S.Textures << " " << S.ListedTextures << " " << S.ZoneFootprint << " "
            << S.ZoneBytes << " " << S.ZoneAssets << " " << (S.Cut ? 1 : 0) << " " << S.Seconds << " " << S.Zone << "\n";
        for (const auto& [E, N] : S.Bytes) Out << "bytes " << (E.empty() ? "-" : E) << " " << N << "\n";
    }
    if (C.SoundHeaders) Out << "soundheaders 1\n";
    for (const auto& [Zone, Rows] : C.Sounds)
    {
        const auto F = C.SoundFootprints.find(Zone);
        Out << "zonesounds " << (F == C.SoundFootprints.end() ? 0 : F->second) << " " << Zone << "\n";
        for (const auto& R : Rows)
            Out << "s " << std::hex << R.Key << std::dec << " " << (int)R.Is << " " << (R.KeptUnderWav ? 1 : 0) << " " << R.Rate << " "
                << R.Channels << " " << R.Stored << " " << R.Frames << " " << R.Bits << "\n";
    }
}

Calibration Measure(const IDiskGame& Game, const ExportOptions& Options, const std::string& Install, const ZoneIndex& Ix,
                    const std::vector<size_t>& Candidates, const std::set<size_t>& Levels, const std::set<Kind>& Wanted,
                    const std::function<void(const std::string&)>& Step, const std::atomic<bool>* Cancel,
                    const Calibration* Previous)
{
    // a listed kind's sample, over at most SampleZones zones: images vary most (a 4096 texture
    // beside a 64 one), models next
    const auto Picked = [](Kind K) -> size_t { return K == Kind::Image ? 96 : K == Kind::Model ? 64 : 32; };
    constexpr size_t SampleZones = 3;
    // another kind's: each of at most WholeZones zones exported until Cap assets or CapSeconds
    constexpr size_t WholeZones = 4, Cap = 24;
    constexpr double CapSeconds = 15, MaxSeconds = 90;
    constexpr uint64_t Opening = 512ull << 20;   // a zone larger than that is opened for a sample only when no other will do

    Calibration C;
    if (Previous != nullptr)
    {
        // the zones whose sound headers were read already: kept, not read again
        C.Sounds = Previous->Sounds;
        C.SoundFootprints = Previous->SoundFootprints;
        C.SoundHeaders = Previous->SoundHeaders;
    }
    C.Formats = FormatsNow();
    for (const auto& [Key, Value] : Options.Settings) C.Settings += (C.Settings.empty() ? "" : ";") + Key + "=" + Value;
    std::vector<size_t> Zones = Candidates;
    if (Zones.empty())
        for (size_t Z = 0; Z < Ix.Zones().size(); Z++) Zones.push_back(Z);
    // zones that did not open have no footprint: no sample from them
    Zones.erase(std::remove_if(Zones.begin(), Zones.end(), [&](size_t Z) { return Ix.Footprint(Z) == 0; }), Zones.end());
    std::sort(Zones.begin(), Zones.end(), [&](size_t A, size_t B) { return Ix.Footprint(A) < Ix.Footprint(B); });
    if (Zones.empty()) return C;

    // the kinds the index lists, and a zone's names of one
    std::set<Kind> Listed;
    for (size_t Z = 0; Z < Ix.Zones().size(); Z++)
        for (const auto& A : Ix.Defined(Z)) Listed.insert(A.K);
    const auto NamesIn = [&](size_t Z, Kind K) {
        std::vector<std::string> Names;
        for (const auto& A : Ix.Defined(Z))
            if (A.K == K) Names.push_back(A.Name);
        return Names;
    };
    const auto Cancelled = [&] { return Cancel != nullptr && Cancel->load(); };
    const auto Join = [&](const std::vector<size_t>& Of) {
        std::string Text;
        for (size_t Z : Of) Text += (Text.empty() ? "" : ", ") + Ix.Zones()[Z];
        return Text;
    };

    const fs::path Root = fs::temp_directory_path() / "greyhound_sizes" / (std::string(Game.Id()) + "_" + std::to_string(GetCurrentProcessId()));
    // One export of K from Of into its own folder, stopped once Enough says (or on Cancel): what
    // it went through, what the reader said the kind holds, what it wrote
    struct SampleRun
    {
        size_t Assets = 0, Count = 0, Done = 0;   // Done of Count: where the reader stopped
        bool Cut = false;
        std::map<std::string, uint64_t> Bytes;
        uint64_t Textures = 0, ListedTextures = 0;
    };
    const auto ExportSample = [&](Kind K, const std::vector<size_t>& Of, std::shared_ptr<const DiskGame::Selection> Only,
                            const std::function<bool(const Progress&, double)>& Enough) {
        SampleRun R;
        std::vector<std::string> Paths;
        for (size_t Z : Of)
        {
            std::vector<std::string> One;
            std::string Error;
            if (Game.ResolveZones(Install, { Ix.Zones()[Z] }, Options, One, Error) && !One.empty()) Paths.push_back(One.front());
        }
        if (Paths.empty()) return R;
        Context Ctx;
        Ctx.OutRoot = (Root / KindName(K)).string();
        Ctx.Kinds = { K };
        // a level's textures are most of what it writes, many of them in other zones
        Ctx.PullDependencies = K == Kind::Map;
        Ctx.Only = std::move(Only);
        std::atomic<bool> Stop{ false };
        const auto Start = std::chrono::steady_clock::now();
        const Totals T = Export(Game, Options, Paths, Ctx, [&](const Progress& P) {
            if (P.FromWatchdog) return;
            if (P.Current == K) R.Count = std::max(R.Count, P.Count), R.Done = std::max(R.Done, P.Done);
            const double Took = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
            if (Cancelled() || Took > (K == Kind::Map ? 3 * MaxSeconds : MaxSeconds) || (Enough && Enough(P, Took))) Stop.store(true);
        }, &Stop);
        // stopped before the kind's end (a run of one zone and kind may end before Export looks)
        R.Cut = (T.Cancelled || Stop.load()) && !(R.Count > 0 && R.Done >= R.Count);
        if (const auto KT = T.Kinds.find(K); KT != T.Kinds.end()) R.Assets = std::max(KT->second.Seen, KT->second.Written);
        Tally(fs::path(Ctx.OutRoot) / KindFolder(K), R.Bytes);
        if (K == Kind::Model || K == Kind::Map)
        {
            // its textures, and of them those the index lists as the zones' images (an image
            // estimate counts them already)
            std::set<std::string> Own;
            for (size_t Z : Of)
                for (const auto& A : Ix.Defined(Z))
                    if (A.K == Kind::Image) Own.insert(ExportNaming::Lower(ExportNaming::SafeFileName(A.Name)));
            std::map<std::string, uint64_t> Images;
            Tally(fs::path(Ctx.OutRoot) / KindFolder(Kind::Image), Images, [&](const fs::path& P, uint64_t N) {
                R.Textures += N;
                if (Own.count(ExportNaming::Lower(P.stem().string()))) R.ListedTextures += N;
            });
        }
        std::error_code Ec;
        fs::remove_all(Ctx.OutRoot, Ec);
        return R;
    };

    // Sounds from their headers: the candidate zones not read yet, in one dry run of the sound
    // kind (Context::OnSoundHeader), each zone's rows kept once the run moved past it; false when
    // the game's readers give no header (they then exported for real: the run is stopped at once)
    const auto SoundsByHeader = [&]() {
        constexpr double Budget = 120;
        std::vector<size_t> Todo;
        for (size_t Z : Zones)
            if (!C.Sounds.count(Ix.Zones()[Z])) Todo.push_back(Z);
        if (Todo.empty()) return C.SoundHeaders;
        std::vector<std::string> Paths;
        std::vector<size_t> Of;   // Paths' zones
        for (size_t Z : Todo)
        {
            std::vector<std::string> One;
            std::string Error;
            if (Game.ResolveZones(Install, { Ix.Zones()[Z] }, Options, One, Error) && !One.empty())
            {
                Paths.push_back(One.front());
                Of.push_back(Z);
            }
        }
        if (Paths.empty()) return C.SoundHeaders;
        Context Ctx;
        Ctx.OutRoot = (Root / "soundheaders").string();
        Ctx.Kinds = { Kind::Sound };
        Ctx.PullDependencies = false;
        std::vector<std::vector<SoundRow>> Got(Paths.size());
        size_t At = 0;               // the zone being read
        std::atomic<bool> Stop{ false };
        const auto Start = std::chrono::steady_clock::now();
        Ctx.OnSoundHeader = [&](const SoundHeader& H) { Got[At].push_back(RowOf(H)); };
        // a reader with no header path of its own: what it hands WriteSound on this thread, its
        // WAV or FLAC header read, nothing decoded or written (its data is still read)
        const fs::path SoundDir = fs::path(Ctx.OutRoot) / KindFolder(Kind::Sound);
        size_t Handed = 0;
        const ExportWriters::DrySounds Dry([&](const ExportWriters::SoundSource& Source, const fs::path& Path) {
            Handed++;
            Got[At].push_back(RowOf(HeaderOf(Source, Path.lexically_relative(SoundDir).string())));
        });
        const auto Done = [&](size_t Upto) {
            // the zones before Upto were read to their end
            for (size_t I = 0; I < Upto && I < Paths.size(); I++)
            {
                const std::string& Name = Ix.Zones()[Of[I]];
                if (C.Sounds.count(Name)) continue;
                C.Sounds[Name] = std::move(Got[I]);
                C.SoundFootprints[Name] = Ix.Footprint(Of[I]);
            }
        };
        if (Step) Step("reading the sounds' headers of " + std::to_string(Paths.size()) + " zone(s)...");
        const Totals T = Export(Game, Options, Paths, Ctx, [&](const Progress& P) {
            if (P.FromWatchdog) return;
            if (P.Zone != At)
            {
                At = std::min(P.Zone, Paths.size() - 1);
                if (Step) Step("reading the sounds' headers: zone " + std::to_string(At + 1) + "/" + std::to_string(Paths.size()) + "...");
            }
            const double Took = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
            if (Cancelled() || Took > Budget) Stop.store(true);
            // sounds written with no header told: a reader that gives none, exporting for real (stopped)
            if (P.Current == Kind::Sound && P.Done >= 64 && Got[At].empty() && At == 0) Stop.store(true);
        }, &Stop);
        const bool Supported = T.Counters.count("sound_headers") != 0 || Handed > 0;
        if (Supported) Done(Stop.load() ? At : Paths.size());
        std::error_code Ec;
        fs::remove_all(Ctx.OutRoot, Ec);
        return C.SoundHeaders = Supported;
    };

    const std::set<Kind> Exported = Game.Kinds();
    for (Kind K : Wanted)
    {
        if (!Exported.count(K) || Cancelled()) continue;
        Sample S;
        S.Listed = Listed.count(K) != 0;
        const auto Start = std::chrono::steady_clock::now();
        // sounds read from their headers need no sample
        if (K == Kind::Sound && SoundsByHeader()) continue;
        if (K == Kind::Map)
        {
            // one level, whole: the smallest of Levels (else of the zones, a few tried in turn
            // until one writes a level), with the textures it pulls from other zones
            std::vector<size_t> Try;
            for (size_t Z : Zones)
                if (Levels.empty() || Levels.count(Z)) Try.push_back(Z);
            if (Levels.empty())
                Try.erase(std::remove_if(Try.begin(), Try.end(), [&](size_t Z) { return Ix.Footprint(Z) < (8ull << 20); }), Try.end());
            for (size_t I = 0; I < Try.size() && I < 4 && S.Assets == 0 && !Cancelled(); I++)
            {
                const size_t Z = Try[I];
                if (Step) Step("exporting the level of " + Ix.Zones()[Z] + "...");
                SampleRun R = ExportSample(K, { Z }, nullptr, nullptr);
                if (R.Assets == 0 || R.Cut) continue;
                S.Assets = R.Assets;
                S.Bytes = std::move(R.Bytes);
                S.Textures = R.Textures;
                S.ListedTextures = R.ListedTextures;
                S.Zone = Ix.Zones()[Z];
                S.ZoneFootprint = Ix.Footprint(Z);
                S.ZoneBytes = Written(S) + S.Textures;
                S.ZoneAssets = 1;
            }
            S.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
            if (S.Assets > 0) C.Kinds[K] = std::move(S);
            continue;
        }
        if (S.Listed)
        {
            // the zones holding most of it among those small enough to open quickly (else the
            // smallest holding any), its names spread over each zone's, as many from a zone as it
            // holds of the kind
            std::vector<std::pair<size_t, size_t>> Holding;   // (count, zone)
            for (size_t Z : Zones)
                if (const size_t N = NamesIn(Z, K).size(); N > 0) Holding.emplace_back(N, Z);
            if (Holding.empty()) continue;   // none of the zones holds it: nothing to measure, nothing to write
            std::stable_sort(Holding.begin(), Holding.end(), [&](const auto& A, const auto& B) {
                const bool OpenA = Ix.Footprint(A.second) <= Opening, OpenB = Ix.Footprint(B.second) <= Opening;
                return OpenA != OpenB ? OpenA : A.first > B.first;
            });
            Holding.resize(std::min(Holding.size(), SampleZones));
            size_t Total = 0;
            for (const auto& [N, Z] : Holding) Total += N;
            auto Only = std::make_shared<DiskGame::Selection>();
            std::vector<size_t> Of;
            for (const auto& [N, Z] : Holding)
            {
                const auto Names = NamesIn(Z, K);
                const size_t Take = std::min(Names.size(), std::max<size_t>(1, Picked(K) * N / Total));
                for (size_t I = 0; I < Take; I++) Only->insert({ K, Names[(2 * I + 1) * Names.size() / (2 * Take)] });
                Of.push_back(Z);
            }
            S.Zone = Join(Of);
            if (Step) Step(std::string("measuring a sample of ") + KindFolder(K) + " in " + S.Zone + "...");
            // a game whose readers write a kind whole ignores the names: its first ones then
            const bool Whole = !Game.FiltersByName();
            SampleRun R = ExportSample(K, Of, Whole ? nullptr : std::move(Only),
                           [&](const Progress& P, double Took) { return Whole && P.Current == K && (P.Done >= Cap || Took > CapSeconds); });
            S.Assets = R.Assets;
            S.Bytes = std::move(R.Bytes);
            S.Textures = R.Textures;
            S.ListedTextures = R.ListedTextures;
            S.Cut = R.Cut;
            for (size_t Z : Of) S.ZoneFootprint += Ix.Footprint(Z);
            S.ZoneBytes = Written(S);
            S.ZoneAssets = R.Count;
        }
        else
        {
            // each picked zone when they are few, else zones spread over their footprints (the
            // largest left out when another will do), each exported whole until the cap: what
            // they wrote per byte of their footprint
            std::vector<size_t> Of;
            if (Zones.size() <= WholeZones) Of = Zones;
            else
                for (size_t I = 0; I < WholeZones; I++)
                {
                    size_t At = (2 * I + 1) * Zones.size() / (2 * WholeZones);
                    while (At > 0 && Ix.Footprint(Zones[At]) > Opening) At--;
                    if (std::find(Of.begin(), Of.end(), Zones[At]) == Of.end()) Of.push_back(Zones[At]);
                }
            S.Zone = Join(Of);
            for (size_t Z : Of)
            {
                if (Cancelled()) break;
                if (Step) Step(std::string("measuring a sample of ") + KindFolder(K) + " in " + Ix.Zones()[Z] + "...");
                SampleRun R = ExportSample(K, { Z }, nullptr, [&](const Progress& P, double Took) { return P.Current == K && (P.Done >= Cap || Took > CapSeconds); });
                uint64_t Bytes = 0;
                for (const auto& [E, N] : R.Bytes)
                {
                    S.Bytes[E] += N;
                    Bytes += N;
                }
                // a zone stopped at the cap: its whole by the count its reader gave
                if (R.Cut && R.Done > 0 && R.Count > R.Done) Bytes = Bytes * R.Count / R.Done;
                S.Cut = S.Cut || R.Cut;
                S.Assets += R.Assets;
                S.ZoneAssets += std::max(R.Count, R.Assets);
                S.ZoneBytes += Bytes;
                S.ZoneFootprint += Ix.Footprint(Z);
            }
        }
        S.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
        if (Cancelled()) break;
        if (S.Assets > 0 || !S.Listed) C.Kinds[K] = std::move(S);
    }
    std::error_code Ec;
    fs::remove_all(Root, Ec);
    return C;
}

Estimate Compute(const Calibration& C, const Picks& S)
{
    Estimate E;
    const auto Then = Parse(C.Formats), Now = Parse(S.Formats);
    const std::string ImgThen = Of(Then, ImageKey), ImgNow = Of(Now, ImageKey);
    for (Kind K : S.Kinds)
    {
        KindEstimate R;
        const auto Measured = C.Kinds.find(K);
        const size_t Count = S.Counts.count(K) ? S.Counts.at(K) : 0;
        if (K == Kind::Map)
        {
            // a sampled level's bytes per byte of its zone's footprint (its textures in the image
            // format chosen, the map settings' share by typical ratios), over the picked levels'
            const uint64_t Footprint = S.LevelsKnown ? S.LevelFootprint : S.Footprint;
            const std::string Where = S.LevelsKnown ? Thousands(S.Levels) + " level(s)" : std::string("the picked zones that hold a level");
            R.Approx = true;
            const auto SettingsThen = Parse(C.Settings);
            const double Settings = MapSettings(S.Settings) / MapSettings(SettingsThen);
            if (Measured != C.Kinds.end() && Measured->second.ZoneFootprint > 0)
            {
                const Sample& M = Measured->second;
                bool Converted = false;
                std::string Why;
                // its textures: those of its own zone are the image kind's when it is ticked
                const uint64_t Textures = S.Kinds.count(Kind::Image) ? M.Textures - std::min(M.Textures, M.ListedTextures) : M.Textures;
                const double Bytes = ((double)Written(M) + FormatBytes((double)Textures, ImageFormats, ImgThen, ImgNow, Converted, Why)) * Settings;
                const double PerByte = Bytes / (double)M.ZoneFootprint;
                R.Bytes = (uint64_t)(PerByte * (double)Footprint);
                R.Basis = Where + " (" + Text(Footprint) + " of zone) at " + std::to_string((int)std::lround(PerByte * 100)) + "% of that, as " +
                          M.Zone + "'s level wrote (" + Text((uint64_t)Bytes) + " for " + Text(M.ZoneFootprint) + ", textures " + Why +
                          (S.Kinds.count(Kind::Image) ? ", its zone's own images left to the image kind" : "") +
                          (Settings != 1.0 ? "; the map settings changed since, by typical ratios" : "") + "); levels vary";
            }
            else
            {
                const double F = MapPerFootprint * MapSettings(S.Settings) / MapSettings({});
                R.Bytes = (uint64_t)((double)Footprint * F);
                R.Basis = Where + ": about " + std::to_string((int)std::lround(F * 100)) + "% of their zones' size in memory (" + Text(Footprint) +
                          "), a typical ratio: no level was sampled";
            }
        }
        else if (K == Kind::Sound && C.SoundHeaders &&
                 std::any_of(S.Picked.begin(), S.Picked.end(), [&](const auto& P) { return C.Sounds.count(P.first) != 0; }))
        {
            // each sound of the picked zones whose headers were read, once per file, in the format
            // chosen (SoundBytes); the other picked zones by the read ones' bytes per footprint
            const std::string Format = Of(Now, SoundKey);
            std::set<uint64_t> Seen;
            uint64_t Bytes = 0, ReadFootprint = 0, OtherFootprint = 0;
            size_t Sounds = 0, Zones = 0, Inexact = 0;
            std::map<std::string, size_t> InexactIn;   // per output format: what is exact in each
            for (const auto& [Zone, Footprint] : S.Picked)
            {
                const auto Rows = C.Sounds.find(Zone);
                if (Rows == C.Sounds.end())
                {
                    OtherFootprint += Footprint;
                    continue;
                }
                Zones++;
                ReadFootprint += Footprint;
                for (const SoundRow& Row : Rows->second)
                {
                    if (!Seen.insert(Row.Key).second) continue;
                    bool Exact = true;
                    Bytes += SoundBytes(Row, Format, Exact);
                    Sounds++;
                    Inexact += !Exact;
                    for (const char* Each : { "WAV", "FLAC", "OPUS" })
                    {
                        bool E = true;
                        SoundBytes(Row, Each, E);
                        InexactIn[Each] += !E;
                    }
                }
            }
            R.Bytes = Bytes;
            R.Approx = Inexact > 0;
            R.Basis = Thousands(Sounds) + " sounds of " + Thousands(Zones) + " zone(s) by their headers (rate, channels, length, size): " +
                      (Inexact == 0 ? std::string("exact in ") + Format
                                    : Thousands(Inexact) + " of them encoded to " + Format + ", by a typical ratio");
            R.Basis += " (";
            for (const char* Each : { "WAV", "FLAC", "OPUS" })
                R.Basis += std::string(Each == std::string("WAV") ? "" : ", ") + Each + (InexactIn[Each] == 0 ? " exact" : " \xE2\x89\x88");
            R.Basis += ")";
            if (OtherFootprint > 0 && ReadFootprint > 0)
            {
                R.Bytes += (uint64_t)((double)Bytes * (double)OtherFootprint / (double)ReadFootprint);
                R.Approx = true;
                R.Basis += "; the picked zones not read yet (" + Text(OtherFootprint) + " of zone) scaled from those (Count assets again to read them)";
            }
        }
        else if (Measured == C.Kinds.end())
        {
            R.Unknown = true;
            R.Basis = "not measured: Count assets again to sample it";
        }
        else
        {
            const Sample& M = Measured->second;
            std::string Why;
            double PerZoneByte = 0, PerAsset = 0;
            // a listed kind's sample, else what the sampled zone wrote whole
            double Bytes = M.Listed ? (double)AllBytes(M) : (double)M.ZoneBytes;
            if (K == Kind::Model) Bytes = FormatsBytes(M, ModelFormats, Then, Now, R.Approx, R.Unknown, Why);
            else if (K == Kind::Anim) Bytes = FormatsBytes(M, AnimFormats, Then, Now, R.Approx, R.Unknown, Why);
            else if (K == Kind::Image) Bytes = FormatBytes(Bytes, ImageFormats, ImgThen, ImgNow, R.Approx, Why);
            else if (K == Kind::Sound) Bytes = FormatBytes(Bytes, SoundFormats, Of(Then, SoundKey), Of(Now, SoundKey), R.Approx, Why);
            if ((K == Kind::Model) && Of(Then, "exportalllods") != Of(Now, "exportalllods"))
            {
                Bytes *= Of(Now, "exportalllods") == "true" ? AllLods : 1.0 / AllLods;
                R.Approx = true;
                Why += std::string(Why.empty() ? "" : "; ") + "all LODs " + (Of(Now, "exportalllods") == "true" ? "added" : "taken off") + " by a typical ratio";
            }
            if (M.Listed)
            {
                // bytes per sampled asset, times the picked zones' count
                PerAsset = M.Assets ? Bytes / (double)M.Assets : 0;
                R.Bytes = (uint64_t)(PerAsset * (double)Count);
                R.Basis = Thousands(Count) + " x " + Text((uint64_t)PerAsset) + " (a sample of " + std::to_string(M.Assets) + " from " + M.Zone +
                          ", " + Why + ")";
            }
            else
            {
                // what the sampled zone wrote, per byte of its footprint, over the picked zones'
                PerZoneByte = M.ZoneFootprint ? Bytes / (double)M.ZoneFootprint : 0;
                R.Bytes = (uint64_t)(PerZoneByte * (double)S.Footprint);
                R.Approx = true;
                R.Basis = "scaled from " + M.Zone + " (" + Text((uint64_t)Bytes) + " for " + Text(M.ZoneFootprint) + " of zone" +
                          (M.Cut ? ", extrapolated from " + std::to_string(M.Assets) + " of its " + std::to_string(M.ZoneAssets) : std::string()) +
                          ") to the picked zones' " + Text(S.Footprint) + (Why.empty() ? "" : "; " + Why);
                if (M.Cut && M.ZoneAssets <= M.Assets) R.Basis += "; its count unknown: at least this";
            }
            // a model's textures: those the image kind does not count already (all of them when it
            // is not ticked; else those the index does not list, streamed from the game's packages)
            const bool Images = S.Kinds.count(Kind::Image) != 0;
            const uint64_t Textures = Images ? M.Textures - std::min(M.Textures, M.ListedTextures) : M.Textures;
            if (K == Kind::Model && Textures > 0 && Of(Now, "exportmodelimg") != "false" && M.Assets > 0)
            {
                bool Converted = false;
                std::string TexWhy;
                const double Tex = FormatBytes((double)Textures, ImageFormats, ImgThen, ImgNow, Converted, TexWhy) / (double)M.Assets;
                R.Bytes += (uint64_t)(Tex * (double)Count);
                R.Approx = true;
                R.Basis += std::string("; their textures") + (Images ? " the images do not list" : "") + " about " + Text((uint64_t)Tex) +
                           " a model (shared ones are written once: fewer)";
            }
        }
        E.Bytes += R.Bytes;
        E.Approx = E.Approx || R.Approx;
        E.Unknown = E.Unknown || R.Unknown;
        E.Kinds[K] = std::move(R);
    }
    return E;
}

std::string Text(uint64_t Bytes)
{
    const char* Units[] = { "B", "KB", "MB", "GB", "TB" };
    double V = (double)Bytes;
    int U = 0;
    while (V >= 1024 && U < 4)
    {
        V /= 1024;
        U++;
    }
    char B[32];
    if (U == 0) snprintf(B, sizeof(B), "%llu B", (unsigned long long)Bytes);
    else snprintf(B, sizeof(B), V < 10 ? "%.1f %s" : "%.0f %s", V, Units[U]);
    return B;
}

uint64_t FreeBytes(const std::string& Folder)
{
    std::error_code Ec;
    fs::path P = fs::absolute(Folder, Ec);
    while (!P.empty() && !fs::exists(P, Ec))
    {
        if (P == P.parent_path()) return 0;
        P = P.parent_path();
    }
    ULARGE_INTEGER Free{};
    if (P.empty() || !GetDiskFreeSpaceExW(P.wstring().c_str(), &Free, nullptr, nullptr)) return 0;
    return Free.QuadPart;
}
}
