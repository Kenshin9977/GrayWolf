// Sole TU of the OatInterop static library, compiled /std:c++latest (C++23, what OAT's headers
// need), so GrayWolf stays C++17. No PCH.

#include "OatInProcess.h"
#include <cctype>                            // std::tolower
#include <type_traits>                       // std::is_same_v, std::decay_t (T5/T6 rawfile templating)
#include <zlib.h>                            // T5 rawfile .gsc/.csc are zlib-deflate (OatDumpRawfilesInProcess);
                                              // plain C header, safe here - unlike Core's Compression.h (C++17-only).

#include "ZoneLoading.h"                     // ZoneLoading::LoadZone
#include "Zone/Zone.h"                       // Zone
#include "Pool/AssetPool.h"                  // ZoneAssetPools
#include "Pool/GlobalAssetPool.h"            // GameGlobalAssetPools - cross-zone name resolution
#include "Pool/XAssetInfo.h"                 // XAssetInfoGeneric
#include "Game/IGame.h"                      // GameId, GameId_Names
#include "Utils/TypeAlignment.h"             // tdef_align32 etc. (the *_Assets.h headers need it first)
#include "Game/IW3/IW3.h"                    // IW3::AssetXModel
#include "Game/IW4/IW4.h"
#include "Game/IW5/IW5.h"
#include "Game/T4/T4.h"
#include "Game/T5/T5.h"
#include "Game/T6/T6.h"
#include "Game/T5/CommonT5.h"                // T5::Common::Vec3UnpackUnitVec (normal decode)
#include "Game/T6/CommonT6.h"                // T6::Common::Vec3UnpackUnitVec/Vec2UnpackTexCoords (world-mesh decode)
#include "XModel/XModelCommon.h"             // XModelCommon
#include "XModel/XModelToCommonConverter.h"  // xmodel::ToCommonConverter

// OAT's full dump pipeline (the pieces Unlinker.cpp drives).
#include "ObjWriting.h"                       // ObjWriting::Configuration, Model/ImageOutputFormat_e
#include "ObjWriter.h"                        // IObjWriter (DumpZone)
#include "IObjLoader.h"                       // IObjLoader (referenced containers)
#include "Game/AutoSearchPaths.h"            // AutoSearchPaths
#include "SearchPath/SharedSearchPaths.h"    // SharedSearchPaths
#include "SearchPath/OutputPathFilesystem.h" // OutputPathFilesystem
#include "Dumping/AssetDumpingContext.h"     // AssetDumpingContext, GdtOutputStream
#include "Obj/Gdt/GdtVersion.h"              // GdtVersion
#include "Zone/Definition/ZoneDefWriter.h"  // IZoneDefWriter (zone_source/<zone>.zone)

#include "AssetPreview.h"   // ExtractIwiToPng (textures from the game's IWDs), ExtractIpakImagesByKey (T6)
#include "Image.h"          // Image::ConvertImageMemory (DDS -> PNG, for T6 embedded-in-fastfile images)
#include "GltfSceneWriter.h" // vertex decode for the map export
#include "MapGlbWriter.h"    // the map scene and its one .glb writer

#include <algorithm>
#include <set>

#include <cmath>
#include <deque>
#include <map>
#include <unordered_map>
#include <vector>
#include <utility>
#include <sstream>
#include <fstream>
#include <functional>
#include <optional>
#include <filesystem>
#include <cstdint>
#include <cstring>

namespace
{
    // A map that iterates in first-insertion order. Keying a std::map by pointer orders it by heap
    // address, which changes from run to run and would make the same map export differ.
    template <class K, class V>
    class FirstSeenMap
    {
    public:
        V& operator[](const K& Key)
        {
            auto [It, Inserted] = Index.emplace(Key, Items.size());
            if (Inserted) Items.emplace_back(Key, V{});
            return Items[It->second].second;
        }
        auto begin() { return Items.begin(); }
        auto end() { return Items.end(); }
        size_t size() const { return Items.size(); }

    private:
        std::deque<std::pair<K, V>> Items;   // a deque keeps the references operator[] handed out
        std::unordered_map<K, size_t> Index;
    };
}

namespace
{
    // Clamp a 0..1 float channel to a 0..255 byte (shared by the SEModel + glTF vertex paths).
    inline uint8_t ToByte(float f) { f = f < 0 ? 0 : (f > 1 ? 1 : f); return (uint8_t)(f * 255.0f + 0.5f); }

    // Copies one OAT vertex's geometry channels (pos/normal/uv/color) into a glTF scene vertex.
    GltfSceneWriter::Vertex ToGltfVertex(const XModelVertex& Sv)
    {
        GltfSceneWriter::Vertex V;
        V.pos[0] = Sv.coordinates[0]; V.pos[1] = Sv.coordinates[1]; V.pos[2] = Sv.coordinates[2];
        V.normal[0] = Sv.normal[0]; V.normal[1] = Sv.normal[1]; V.normal[2] = Sv.normal[2];
        V.uv[0] = Sv.uv[0]; V.uv[1] = Sv.uv[1];
        V.color[0] = ToByte(Sv.color[0]); V.color[1] = ToByte(Sv.color[1]);
        V.color[2] = ToByte(Sv.color[2]); V.color[3] = ToByte(Sv.color[3]);
        return V;
    }

    // Walks up from a fastfile path to the game install root (the dir that has a "main" folder).
    std::string FindInstallDir(const std::string& FastFilePath)
    {
        std::error_code Ec;
        std::filesystem::path P = std::filesystem::path(FastFilePath).parent_path();
        for (int i = 0; i < 6 && !P.empty(); i++)
        {
            if (std::filesystem::exists(P / "main", Ec))
                return P.string();
            P = P.parent_path();
        }
        return "";
    }

    // Loads a fastfile's zone (the shared front half of every Oat*InProcess entry point). On failure
    // fills Report and returns null; on success moves the owned zone out of the loader result.
    std::unique_ptr<Zone> LoadZoneOrReport(const std::string& FastFilePath, std::string& Report)
    {
        auto Result = ZoneLoading::LoadZone(FastFilePath, std::nullopt);
        if (!Result.has_value())
        {
            Report = "load failed: " + Result.error();
            return nullptr;
        }
        std::unique_ptr<Zone> Z = std::move(Result.value());
        if (!Z)
            Report = "load returned null zone";
        return Z;
    }

    // Some assets resolve by name across zones (FxImpactTable's "table" is null when a map zone
    // loads alone). GameGlobalAssetPools links every live Zone's pools into one name-keyed table
    // (AssetPool's ctor/dtor), so loading the companion zones alongside and keeping them alive is
    // enough. Best-effort: a missing companion is skipped.
    std::vector<std::unique_ptr<Zone>> LoadCompanionZones(const std::string& FastFilePath)
    {
        std::vector<std::unique_ptr<Zone>> Companions;
        std::error_code Ec;
        const std::filesystem::path RequestedPath(FastFilePath);
        const std::filesystem::path Dir = RequestedPath.parent_path();
        const std::string RequestedStem = RequestedPath.stem().string();
        const bool IsMp = RequestedStem.size() >= 3
            && RequestedStem.compare(RequestedStem.size() - 3, 3, "_mp") == 0;

        const char* CompanionNames[] = { IsMp ? "code_post_gfx_mp" : "code_post_gfx",
                                          IsMp ? "common_mp" : "common" };
        for (const char* Name : CompanionNames)
        {
            if (RequestedStem == Name)
                continue; // don't reload the zone we were already asked to dump
            const std::filesystem::path CompanionPath = Dir / (std::string(Name) + ".ff");
            if (!std::filesystem::exists(CompanionPath, Ec))
                continue;
            auto Result = ZoneLoading::LoadZone(CompanionPath.string(), std::nullopt);
            if (Result.has_value() && Result.value())
                Companions.push_back(std::move(Result.value()));
        }
        return Companions;
    }
}

namespace
{
    ModelOutputFormat_e ParseModelFormat(const std::string& S)
    {
        if (S == "xmodel_export") return ModelOutputFormat_e::XMODEL_EXPORT;
        if (S == "xmodel_bin")    return ModelOutputFormat_e::XMODEL_BIN;
        if (S == "obj")           return ModelOutputFormat_e::OBJ;
        if (S == "glb")           return ModelOutputFormat_e::GLB;
        return ModelOutputFormat_e::GLTF;
    }

    ImageOutputFormat_e ParseImageFormat(const std::string& S)
    {
        if (S == "iwi") return ImageOutputFormat_e::IWI;
        return ImageOutputFormat_e::DDS;
    }

    // Per-zone T6 (BO2) image extraction, for streamed images OAT's own dumper leaves behind: a T6
    // GfxImage can carry pixels in the fastfile or stream them from the install's .ipak packages, and
    // OAT only writes the first kind. Written as PNG beside OAT's DDS via IPAKCache::ExtractByOffset
    // + CoDIWITranslator.
    size_t ExtractT6ZoneImages(const Zone& Z, const std::string& OutDir, const std::string& IpakDir)
    {
        namespace fs = std::filesystem;

        const fs::path ImageDir = fs::path(OutDir) / "images";
        std::vector<std::pair<uint64_t, std::string>> Jobs;
        std::set<std::string> Seen;
        size_t Total = 0, Streamed = 0, NoSize = 0, AlreadyDumped = 0;

        for (auto* Info : Z.m_pools.PoolAssets<T6::AssetImage>())
        {
            Total++;
            const T6::GfxImage* Img = Info ? Info->Asset() : nullptr;
            if (!Img || !Img->name || !*Img->name)
                continue;
            if (Img->name[0] == '$')    // "$white", "$identitynormalmap" - engine defaults
                continue;
            if (Img->streamedPartCount <= 0)
                continue;               // pixels are in the fastfile; OAT has already written it
            Streamed++;

            // GfxStreamedPartInfo's offset/ipakIndex are ZERO in a zone read offline (the engine fills
            // them in at load time); what IS present is the hash pair the IPAK keys its entries by:
            // (GfxImage.hash << 32) | part.hash. So key by that instead of by offset.
            const T6::GfxStreamedPartInfo& Part = Img->streamedParts[0];
            if (Part.hash == 0 || Img->hash == 0)
            {
                NoSize++;
                continue;
            }

            std::string Name(Img->name);
            for (auto& C : Name)
                if (C == '\\' || C == '/' || C == ':' || C == '*' || C == '?' || C == '"' ||
                    C == '<' || C == '>' || C == '|')
                    C = '_';
            if (!Seen.insert(Name).second)
                continue;

            std::error_code Ec;
            if (fs::exists(ImageDir / (Name + ".dds"), Ec))
            {
                AlreadyDumped++;
                continue;               // OAT got this one out of the fastfile
            }

            Jobs.emplace_back((static_cast<uint64_t>(Img->hash) << 32) |
                              static_cast<uint64_t>(Part.hash),
                              (ImageDir / (Name + ".png")).string());
        }

        printf("T6 images: %zu total, %zu streamed (%zu with no usable part, %zu already written "
               "by OAT), %zu to fetch from %s\n",
               Total, Streamed, NoSize, AlreadyDumped, Jobs.size(), IpakDir.c_str());

        if (Jobs.empty())
            return 0;

        return ExtractIpakImagesByKey(IpakDir, Jobs);
    }

    // Per-zone T5 (BO1) sound extraction, reusing OAT's already-loaded SndBank pool: each alias is
    // either a LoadedSound (RIFF/WAVE embedded in the .ff, written directly - what OAT's T4
    // LoadedSoundDumper does, just never wired for T5) or a StreamedSound (filename decoded from the
    // install's IWDs). Extracts only the sounds this fastfile references.
    size_t ExtractT5ZoneSounds(const Zone& Z, const std::string& OutDir, const std::string& InstallDir)
    {
        size_t Written = 0;
        std::set<std::string> Seen;

        auto IsRiffWave = [](const void* Data, unsigned N) {
            const auto* B = reinterpret_cast<const unsigned char*>(Data);
            return Data && N >= 12 && B[0]=='R'&&B[1]=='I'&&B[2]=='F'&&B[3]=='F'
                                   && B[8]=='W'&&B[9]=='A'&&B[10]=='V'&&B[11]=='E';
        };
        auto Slashes = [](std::string S) { for (auto& C : S) if (C == '\\') C = '/'; return S; };
        auto EnsureWav = [](std::string S) {
            if (S.size() < 4 || S.compare(S.size()-4, 4, ".wav") != 0) S += ".wav";
            return S;
        };

        for (auto* Info : Z.m_pools.PoolAssets<T5::AssetSoundBank>())
        {
            const T5::SndBank* Bank = Info ? Info->Asset() : nullptr;
            if (!Bank || !Bank->alias)
                continue;

            for (unsigned i = 0; i < Bank->aliasCount; i++)
            {
                const T5::snd_alias_list_t& List = Bank->alias[i];
                if (!List.head)
                    continue;

                for (int j = 0; j < List.count; j++)
                {
                    const T5::snd_alias_t& A = List.head[j];
                    const T5::SoundFile* SF = A.soundFile;
                    if (!SF)
                        continue;

                    if ((unsigned char)SF->type == T5::SAT_LOADED && SF->u.loadSnd)
                    {
                        const T5::LoadedSound* LS = SF->u.loadSnd;
                        if (!LS->sound.data || LS->sound.data_size == 0)
                            continue;
                        if (!IsRiffWave(LS->sound.data, LS->sound.data_size))
                            continue; // only pass through complete RIFF/WAVE (matches OAT's T4 dumper)

                        std::string Name = LS->name ? Slashes(LS->name) : (A.name ? Slashes(A.name) : std::string());
                        if (Name.empty())
                            continue;
                        Name = EnsureWav(Name);
                        if (!Seen.insert(Name).second)
                            continue;
                        try
                        {
                            const std::filesystem::path Out = std::filesystem::path(OutDir) / Name;
                            std::error_code Ec;
                            std::filesystem::create_directories(Out.parent_path(), Ec);
                            std::ofstream O(Out, std::ios::binary | std::ios::trunc);
                            if (O)
                            {
                                O.write(reinterpret_cast<const char*>(LS->sound.data), (std::streamsize)LS->sound.data_size);
                                if (O) Written++;
                            }
                        }
                        catch (const std::exception&) {}
                    }
                    else if ((unsigned char)SF->type == T5::SAT_STREAMED && SF->u.streamSnd && !InstallDir.empty())
                    {
                        const T5::StreamedSound* SS = SF->u.streamSnd;
                        if (!SS->filename)
                            continue;
                        std::string Entry = EnsureWav(Slashes(SS->filename));
                        if (Entry.empty())
                            continue;
                        if (!Seen.insert(Entry).second)
                            continue;
                        if (DecodeIwdSoundByName(InstallDir, Entry, (std::filesystem::path(OutDir) / Entry).string()))
                            Written++;
                    }
                }
            }
        }
        return Written;
    }

    std::string JsonEscape(const std::string& S)
    {
        // Every control char (0x00-0x1F) is illegal unescaped in JSON, not just \n/\r/\\/\"; names
        // are not guaranteed ASCII. \uXXXX-escape the ones without a short mnemonic.
        static const char* Hex = "0123456789abcdef";
        std::string R;
        R.reserve(S.size() + 4);
        for (unsigned char C : S)
        {
            switch (C)
            {
            case '"':  R += "\\\""; break;
            case '\\': R += "\\\\"; break;
            case '\n': R += "\\n";  break;
            case '\r': R += "\\r";  break;
            case '\t': R += "\\t";  break;
            case '\b': R += "\\b";  break;
            case '\f': R += "\\f";  break;
            default:
                if (C < 0x20)
                {
                    R += "\\u00";
                    R += Hex[(C >> 4) & 0xF];
                    R += Hex[C & 0xF];
                }
                else
                {
                    R += (char)C;
                }
                break;
            }
        }
        return R;
    }

    // Every name/alias pointer from a loaded zone goes through here: a union member read as a
    // string can hold garbage that happens to null-terminate, so the scan is bounded and anything
    // outside printable ASCII is rejected.
    std::string SafeCString(const char* P, size_t MaxLen = 256)
    {
        if (!P || (const void*)P < (const void*)0x10000) // reject null / small-int-misread-as-pointer
            return "";
        for (size_t i = 0; i < MaxLen; i++)
        {
            const unsigned char C = (unsigned char)P[i];
            if (C == 0)
                return std::string(P, i);
            if (C < 0x20 || C > 0x7E)
                return ""; // not printable ASCII - treat as absent rather than emit raw bytes
        }
        return ""; // no terminator within MaxLen - not a real short asset name, bail out
    }

    // JSON-quotes a possibly-null asset-name pointer, through the same SafeCString discipline as
    // every other name in this file - "null" (unquoted, a real JSON null) rather than "" for an
    // absent/garbage pointer, so a consumer can tell "no burn effect" from "effect with an empty name".
    std::string NameJson(const char* N)
    {
        const std::string S = SafeCString(N);
        return S.empty() ? "null" : ("\"" + JsonEscape(S) + "\"");
    }

    // destructible_def (ASSET_TYPE_DESTRUCTIBLEDEF): OAT's generated loader is complete for this type
    // in both games, but neither game's ObjWriterT{5,6}.cpp has the REGISTER_DUMPER line uncommented
    // - DumpZone loads every one correctly and never writes one to disk. T5::DestructibleDef and
    // T6::DestructibleDef are byte-identical today but are separate structs/namespaces, hence templated.
    template <typename TAssetDestructibleDef>
    size_t ExportDestructibleDefPool(Zone& Z, AssetDumpingContext& Context)
    {
        size_t Written = 0;
        for (auto* Info : Z.m_pools.template PoolAssets<TAssetDestructibleDef>())
        {
            const auto* Def = Info ? Info->Asset() : nullptr;
            const std::string Name = Def ? SafeCString(Def->name) : "";
            if (Name.empty())
                continue;

            const auto AssetFile = Context.OpenAssetFile("destructible_def/" + Name + ".json");
            if (!AssetFile)
                continue;
            auto& Out = *AssetFile;

            Out << "{\n";
            Out << "  \"name\": " << NameJson(Def->name) << ",\n";
            Out << "  \"model\": " << NameJson(Def->model ? Def->model->name : nullptr) << ",\n";
            Out << "  \"pristineModel\": " << NameJson(Def->pristineModel ? Def->pristineModel->name : nullptr) << ",\n";
            Out << "  \"clientOnly\": " << (Def->clientOnly ? "true" : "false") << ",\n";
            Out << "  \"numPieces\": " << Def->numPieces << ",\n";
            Out << "  \"pieces\": [\n";

            const int NumPieces = (Def->numPieces > 0 && Def->pieces) ? Def->numPieces : 0;
            for (int p = 0; p < NumPieces; p++)
            {
                const auto& Pc = Def->pieces[p];
                Out << "    {\n";
                Out << "      \"parentPiece\": " << (int)Pc.parentPiece << ",\n";
                Out << "      \"parentDamagePercent\": " << Pc.parentDamagePercent << ",\n";
                Out << "      \"bulletDamageScale\": " << Pc.bulletDamageScale << ",\n";
                Out << "      \"explosiveDamageScale\": " << Pc.explosiveDamageScale << ",\n";
                Out << "      \"meleeDamageScale\": " << Pc.meleeDamageScale << ",\n";
                Out << "      \"impactDamageScale\": " << Pc.impactDamageScale << ",\n";
                Out << "      \"entityDamageTransfer\": " << Pc.entityDamageTransfer << ",\n";
                Out << "      \"physConstraints\": " << NameJson(Pc.physConstraints ? Pc.physConstraints->name : nullptr) << ",\n";
                Out << "      \"health\": " << Pc.health << ",\n";
                Out << "      \"damageSound\": " << NameJson(Pc.damageSound) << ",\n";
                Out << "      \"burnEffect\": " << NameJson(Pc.burnEffect ? Pc.burnEffect->name : nullptr) << ",\n";
                Out << "      \"burnSound\": " << NameJson(Pc.burnSound) << ",\n";
                Out << "      \"enableLabel\": " << (unsigned)Pc.enableLabel << ",\n";
                // hideBones is a 5*32=160-bit mask (one bit per bone slot to hide on break), not an
                // int array of bone indices despite the field name/type; printed as hex per word, not signed
                // decimal (e.g. -1073741824).
                Out << "      \"hideBonesMask\": [";
                {
                    // 13 bytes needed ("0x" + 8 hex digits + 2 quotes + terminator).
                    char Hex8[16];
                    for (int h = 0; h < 5; h++)
                    {
                        snprintf(Hex8, sizeof(Hex8), "\"0x%08X\"", (unsigned)Pc.hideBones[h]);
                        Out << Hex8 << (h < 4 ? "," : "");
                    }
                }
                Out << "],\n";
                Out << "      \"stages\": [\n";
                for (int s = 0; s < 5; s++)
                {
                    const auto& St = Pc.stages[s];
                    Out << "        {\n";
                    Out << "          \"showBone\": " << (unsigned)St.showBone << ",\n";
                    Out << "          \"breakHealth\": " << St.breakHealth << ",\n";
                    Out << "          \"maxTime\": " << St.maxTime << ",\n";
                    Out << "          \"flags\": " << St.flags << ",\n";
                    Out << "          \"breakEffect\": " << NameJson(St.breakEffect ? St.breakEffect->name : nullptr) << ",\n";
                    Out << "          \"breakSound\": " << NameJson(St.breakSound) << ",\n";
                    Out << "          \"breakNotify\": " << NameJson(St.breakNotify) << ",\n";
                    Out << "          \"loopSound\": " << NameJson(St.loopSound) << ",\n";
                    Out << "          \"spawnModel\": [";
                    for (int m = 0; m < 3; m++)
                        Out << NameJson(St.spawnModel[m] ? St.spawnModel[m]->name : nullptr) << (m < 2 ? "," : "");
                    Out << "],\n";
                    Out << "          \"physPreset\": " << NameJson(St.physPreset ? St.physPreset->name : nullptr) << "\n";
                    Out << "        }" << (s < 4 ? "," : "") << "\n";
                }
                Out << "      ]\n";
                Out << "    }" << (p + 1 < NumPieces ? "," : "") << "\n";
            }

            Out << "  ]\n";
            Out << "}\n";
            Written++;
        }
        return Written;
    }

    // Writes a fixed-size float vector (vec2_t/vec3_t/vec4_t - all unions with a ".v[N]" member in
    // both T5_Assets.h and T6_Assets.h - or a plain "float[N]", the one remaining case being T5's
    // XGlobals::screenClearColor) as a JSON array, uniformly across both representations.
    template <size_t N, typename TVec>
    void WriteFloatVec(std::ostream& Out, const TVec& V)
    {
        Out << "[";
        for (size_t i = 0; i < N; i++)
        {
            if constexpr (requires { V.v[i]; })
                Out << V.v[i];
            else
                Out << V[i];
            Out << (i + 1 < N ? "," : "");
        }
        Out << "]";
    }

    // xglobals (ASSET_TYPE_XGLOBALS) - loader complete both games (T5_Assets.h:5247/T6_Assets.h:1968),
    // no writer registered in either ObjWriterT5.cpp/ObjWriterT6.cpp. T6 adds gumpReserve->(dropped)
    // and a fixed 32-slot gump/overlay info table (only gumpsCount/overlayCount entries are live, the
    // rest is unused padding in the fixed array - only those are written).
    template <typename TAssetXGlobals>
    size_t ExportXGlobalsPool(Zone& Z, AssetDumpingContext& Context)
    {
        size_t Written = 0;
        for (auto* Info : Z.m_pools.template PoolAssets<TAssetXGlobals>())
        {
            const auto* X = Info ? Info->Asset() : nullptr;
            const std::string Name = X ? SafeCString(X->name) : "";
            if (Name.empty())
                continue;
            const auto AssetFile = Context.OpenAssetFile("xglobals/" + Name + ".json");
            if (!AssetFile) continue;
            auto& Out = *AssetFile;

            Out << "{\n";
            Out << "  \"name\": " << NameJson(X->name) << ",\n";
            Out << "  \"xanimStreamBufferSize\": " << X->xanimStreamBufferSize << ",\n";
            Out << "  \"cinematicMaxWidth\": " << X->cinematicMaxWidth << ",\n";
            Out << "  \"cinematicMaxHeight\": " << X->cinematicMaxHeight << ",\n";
            Out << "  \"extracamResolution\": " << X->extracamResolution << ",\n";
            if constexpr (requires { X->gumpReserve; })
                Out << "  \"gumpReserve\": " << X->gumpReserve << ",\n";
            Out << "  \"screenClearColor\": ";
            WriteFloatVec<4>(Out, X->screenClearColor);
            if constexpr (requires { X->gumps; })
            {
                Out << ",\n  \"gumps\": [";
                for (int i = 0; i < X->gumpsCount && i < 32; i++)
                    Out << (i ? "," : "") << "{ \"name\": " << NameJson(X->gumps[i].name)
                        << ", \"size\": " << X->gumps[i].size << " }";
                Out << "],\n";
                Out << "  \"overlays\": [";
                for (int i = 0; i < X->overlayCount && i < 32; i++)
                    Out << (i ? "," : "") << "{ \"name\": " << NameJson(X->overlays[i].name)
                        << ", \"size\": " << X->overlays[i].size << " }";
                Out << "]\n";
            }
            else
            {
                Out << "\n";
            }
            Out << "}\n";
            Written++;
        }
        return Written;
    }

    // snd_driver_globals - field NAMES diverge between games (snapshotGroups vs duckGroups, plus two
    // T6-only arrays), not just extra fields, so this is two plain functions rather than one
    // templated one with heavy if-constexpr branching over different member names.
    size_t ExportSndDriverGlobalsPoolT5(Zone& Z, AssetDumpingContext& Context)
    {
        size_t Written = 0;
        for (auto* Info : Z.m_pools.PoolAssets<T5::AssetSoundDriverGlobals>())
        {
            const auto* S = Info ? Info->Asset() : nullptr;
            const std::string Name = S ? SafeCString(S->name) : "";
            if (Name.empty())
                continue;
            const auto AssetFile = Context.OpenAssetFile("snd_driver_globals/" + Name + ".json");
            if (!AssetFile) continue;
            auto& Out = *AssetFile;

            Out << "{\n";
            Out << "  \"name\": " << NameJson(S->name) << ",\n";
            Out << "  \"groupCount\": " << S->groupCount << ",\n";
            Out << "  \"groups\": [";
            for (unsigned i = 0; i < S->groupCount; i++)
                Out << (i ? "," : "") << NameJson(S->groups ? S->groups[i].name : nullptr);
            Out << "],\n";
            Out << "  \"curveCount\": " << S->curveCount << ",\n";
            Out << "  \"panCount\": " << S->panCount << ",\n";
            Out << "  \"snapshotGroupCount\": " << S->snapshotGroupCount << ",\n";
            Out << "  \"snapshotGroups\": [";
            for (unsigned i = 0; i < S->snapshotGroupCount; i++)
                Out << (i ? "," : "") << "\"" << JsonEscape(S->snapshotGroups
                    ? SafeCString(S->snapshotGroups[i].name, 32) : "") << "\"";
            Out << "],\n";
            Out << "  \"contextCount\": " << S->contextCount << ",\n";
            Out << "  \"masterCount\": " << S->masterCount << "\n";
            Out << "}\n";
            Written++;
        }
        return Written;
    }

    size_t ExportSndDriverGlobalsPoolT6(Zone& Z, AssetDumpingContext& Context)
    {
        size_t Written = 0;
        for (auto* Info : Z.m_pools.PoolAssets<T6::AssetSoundDriverGlobals>())
        {
            const auto* S = Info ? Info->Asset() : nullptr;
            const std::string Name = S ? SafeCString(S->name) : "";
            if (Name.empty())
                continue;
            const auto AssetFile = Context.OpenAssetFile("snd_driver_globals/" + Name + ".json");
            if (!AssetFile) continue;
            auto& Out = *AssetFile;

            Out << "{\n";
            Out << "  \"name\": " << NameJson(S->name) << ",\n";
            Out << "  \"groupCount\": " << S->groupCount << ",\n";
            Out << "  \"groups\": [";
            for (unsigned i = 0; i < S->groupCount; i++)
                Out << (i ? "," : "") << NameJson(S->groups ? S->groups[i].name : nullptr);
            Out << "],\n";
            Out << "  \"curveCount\": " << S->curveCount << ",\n";
            Out << "  \"panCount\": " << S->panCount << ",\n";
            Out << "  \"duckGroupCount\": " << S->duckGroupCount << ",\n";
            Out << "  \"duckGroups\": [";
            for (unsigned i = 0; i < S->duckGroupCount; i++)
                Out << (i ? "," : "") << "\"" << JsonEscape(S->duckGroups
                    ? SafeCString(S->duckGroups[i].name, 32) : "") << "\"";
            Out << "],\n";
            Out << "  \"contextCount\": " << S->contextCount << ",\n";
            Out << "  \"masterCount\": " << S->masterCount << ",\n";
            Out << "  \"voiceDuckCount\": " << S->voiceDuckCount << ",\n";
            Out << "  \"futzCount\": " << S->futzCount << "\n";
            Out << "}\n";
            Written++;
        }
        return Written;
    }

    // glasses (+ nested GlassDef/Glass). Glasses::workMemory is a preallocated runtime scratch
    // buffer (sized by workMemorySize) - not real asset data, intentionally not written. Glass
    // diverges only by T6 ADDING numCellIndices/cellIndices/binormalSign (if-constexpr-gated), every
    // other field is identical both games.
    template <typename TAssetGlasses>
    size_t ExportGlassesPool(Zone& Z, AssetDumpingContext& Context)
    {
        size_t Written = 0;
        for (auto* Info : Z.m_pools.template PoolAssets<TAssetGlasses>())
        {
            const auto* G = Info ? Info->Asset() : nullptr;
            const std::string Name = G ? SafeCString(G->name) : "";
            if (Name.empty())
                continue;
            const auto AssetFile = Context.OpenAssetFile("glasses/" + Name + ".json");
            if (!AssetFile) continue;
            auto& Out = *AssetFile;

            Out << "{\n";
            Out << "  \"name\": " << NameJson(G->name) << ",\n";
            Out << "  \"numGlasses\": " << G->numGlasses << ",\n";
            Out << "  \"smallAllocatorBlocks\": " << G->smallAllocatorBlocks << ",\n";
            Out << "  \"maxGroups\": " << G->maxGroups << ",\n";
            Out << "  \"maxShards\": " << G->maxShards << ",\n";
            Out << "  \"maxPhysics\": " << G->maxPhysics << ",\n";
            Out << "  \"shardMemorySize\": " << G->shardMemorySize << ",\n";
            Out << "  \"maxFreeCmd\": " << G->maxFreeCmd << ",\n";
            Out << "  \"numSlots\": " << G->numSlots << ",\n";
            Out << "  \"numVerts\": " << G->numVerts << ",\n";
            Out << "  \"numIndices\": " << G->numIndices << ",\n";
            Out << "  \"glasses\": [\n";
            for (unsigned i = 0; i < G->numGlasses; i++)
            {
                const auto& Gl = G->glasses[i];
                Out << "    {\n";
                Out << "      \"glassDef\": " << NameJson(Gl.glassDef ? Gl.glassDef->name : nullptr) << ",\n";
                if constexpr (requires { Gl.numCellIndices; })
                {
                    Out << "      \"numCellIndices\": " << Gl.numCellIndices << ",\n";
                    Out << "      \"binormalSign\": " << (int)Gl.binormalSign << ",\n";
                }
                Out << "      \"index\": " << Gl.index << ",\n";
                Out << "      \"brushModel\": " << Gl.brushModel << ",\n";
                Out << "      \"origin\": "; WriteFloatVec<3>(Out, Gl.origin); Out << ",\n";
                Out << "      \"angles\": "; WriteFloatVec<3>(Out, Gl.angles); Out << ",\n";
                Out << "      \"isPlanar\": " << (Gl.isPlanar ? "true" : "false") << ",\n";
                Out << "      \"numOutlineVerts\": " << (int)Gl.numOutlineVerts << ",\n";
                Out << "      \"uvScale\": " << Gl.uvScale << ",\n";
                Out << "      \"thickness\": " << Gl.thickness << "\n";
                Out << "    }" << (i + 1 < G->numGlasses ? "," : "") << "\n";
            }
            Out << "  ]\n";
            Out << "}\n";
            Written++;
        }
        return Written;
    }

    // emblem_set. T6 adds iconTypes/bgCategories tables and diverges EmblemIcon (drops
    // defaultColor, adds iconType) / EmblemBackground (adds sortKey/bgCategory/mtxIndex) -
    // if-constexpr-gated per field, same pattern as everything else in this file.
    template <typename TAssetEmblemSet>
    size_t ExportEmblemSetPool(Zone& Z, AssetDumpingContext& Context)
    {
        size_t Written = 0;
        for (auto* Info : Z.m_pools.template PoolAssets<TAssetEmblemSet>())
        {
            const auto* E = Info ? Info->Asset() : nullptr;
            if (!E)
                continue;
            // EmblemSet has no name field of its own (it's a singleton per zone) - key the file
            // on the zone name instead, passed in by the caller via Z.m_name.
            const auto AssetFile = Context.OpenAssetFile("emblem_set/" + Z.m_name + ".json");
            if (!AssetFile) continue;
            auto& Out = *AssetFile;

            Out << "{\n";
            Out << "  \"colorCount\": " << E->colorCount << ",\n";
            Out << "  \"layerCount\": " << E->layerCount << ",\n";
            Out << "  \"layers\": [";
            for (int i = 0; i < E->layerCount; i++)
                Out << (i ? "," : "") << "{ \"cost\": " << E->layers[i].cost
                    << ", \"unlockLevel\": " << E->layers[i].unlockLevel
                    << ", \"unlockPLevel\": " << E->layers[i].unlockPLevel << " }";
            Out << "],\n";
            Out << "  \"categoryCount\": " << E->categoryCount << ",\n";
            Out << "  \"categories\": [";
            for (int i = 0; i < E->categoryCount; i++)
                Out << (i ? "," : "") << "{ \"name\": " << NameJson(E->categories[i].name)
                    << ", \"description\": " << NameJson(E->categories[i].description) << " }";
            Out << "],\n";
            if constexpr (requires { E->iconTypeCount; })
            {
                Out << "  \"iconTypeCount\": " << E->iconTypeCount << ",\n";
                Out << "  \"iconTypes\": [";
                for (int i = 0; i < E->iconTypeCount; i++)
                    Out << (i ? "," : "") << "{ \"name\": " << NameJson(E->iconTypes[i].name)
                        << ", \"description\": " << NameJson(E->iconTypes[i].description) << " }";
                Out << "],\n";
                Out << "  \"bgCategoryCount\": " << E->bgCategoryCount << ",\n";
                Out << "  \"bgCategories\": [";
                for (int i = 0; i < E->bgCategoryCount; i++)
                    Out << (i ? "," : "") << "{ \"name\": " << NameJson(E->bgCategories[i].name)
                        << ", \"description\": " << NameJson(E->bgCategories[i].description) << " }";
                Out << "],\n";
            }
            Out << "  \"iconCount\": " << E->iconCount << ",\n";
            Out << "  \"icons\": [";
            for (int i = 0; i < E->iconCount; i++)
            {
                const auto& Ic = E->icons[i];
                Out << (i ? "," : "") << "{ \"image\": " << NameJson(Ic.image ? Ic.image->name : nullptr)
                    << ", \"description\": " << NameJson(Ic.description)
                    << ", \"outlineSize\": " << Ic.outlineSize
                    << ", \"cost\": " << Ic.cost << ", \"unlockLevel\": " << Ic.unlockLevel
                    << ", \"unlockPLevel\": " << Ic.unlockPLevel << ", \"unclassifyAt\": " << Ic.unclassifyAt
                    << ", \"sortKey\": " << Ic.sortKey;
                if constexpr (requires { Ic.defaultColor; })
                    Out << ", \"defaultColor\": " << Ic.defaultColor;
                if constexpr (requires { Ic.iconType; })
                    Out << ", \"iconType\": " << Ic.iconType;
                Out << ", \"category\": " << (unsigned)Ic.category << " }";
            }
            Out << "],\n";
            Out << "  \"backgroundCount\": " << E->backgroundCount << ",\n";
            Out << "  \"backgrounds\": [";
            for (int i = 0; i < E->backgroundCount; i++)
            {
                const auto& Bg = E->backgrounds[i];
                Out << (i ? "," : "") << "{ \"material\": " << NameJson(Bg.material ? Bg.material->info.name : nullptr)
                    << ", \"description\": " << NameJson(Bg.description) << ", \"cost\": " << Bg.cost
                    << ", \"unlockLevel\": " << Bg.unlockLevel << ", \"unlockPLevel\": " << Bg.unlockPLevel
                    << ", \"unclassifyAt\": " << Bg.unclassifyAt;
                if constexpr (requires { Bg.sortKey; })
                    Out << ", \"sortKey\": " << Bg.sortKey << ", \"bgCategory\": " << Bg.bgCategory
                        << ", \"mtxIndex\": " << Bg.mtxIndex;
                Out << " }";
            }
            Out << "],\n";
            Out << "  \"backgroundLookupCount\": " << E->backgroundLookupCount << ",\n";
            Out << "  \"backgroundLookup\": [";
            for (int i = 0; i < E->backgroundLookupCount; i++)
                Out << (i ? "," : "") << (E->backgroundLookup ? E->backgroundLookup[i] : (int16_t)0);
            Out << "]\n";
            Out << "}\n";
            Written++;
        }
        return Written;
    }

    // pack_index (T5-only asset type - no T6 equivalent at all). Fully flat: a header plus a hash
    // table of (hash, offset, size) triples, no asset pointers, no blobs.
    size_t ExportPackIndexPool(Zone& Z, AssetDumpingContext& Context)
    {
        size_t Written = 0;
        for (auto* Info : Z.m_pools.PoolAssets<T5::AssetPackIndex>())
        {
            const auto* P = Info ? Info->Asset() : nullptr;
            const std::string Name = P ? SafeCString(P->name) : "";
            if (Name.empty())
                continue;
            const auto AssetFile = Context.OpenAssetFile("pack_index/" + Name + ".json");
            if (!AssetFile) continue;
            auto& Out = *AssetFile;

            Out << "{\n";
            Out << "  \"name\": " << NameJson(P->name) << ",\n";
            Out << "  \"header\": { \"magic\": " << P->header.magic << ", \"timestamp\": " << P->header.timestamp
                << ", \"count\": " << P->header.count << ", \"alignment\": " << P->header.alignment
                << ", \"data_start\": " << P->header.data_start << " },\n";
            Out << "  \"entries\": [";
            for (unsigned i = 0; i < P->header.count; i++)
                Out << (i ? "," : "") << "{ \"hash\": " << P->entries[i].hash
                    << ", \"offset\": " << P->entries[i].offset << ", \"size\": " << P->entries[i].size << " }";
            Out << "]\n";
            Out << "}\n";
            Written++;
        }
        return Written;
    }

    // fx_impact_table. "table" is a pointer to a SINGLE FxImpactEntry (not an array) holding every
    // surface-type -> effect mapping via its own fixed nonflesh[]/flesh[4] arrays (sizes diverge, 31
    // vs 32 nonflesh slots - read via sizeof/array bound, not hardcoded). A map zone dumped alone
    // has a named FxImpactTable whose "table" is null (the data lives in a companion zone, see
    // LoadCompanionZones): if unresolved locally, re-query the name through GameGlobalAssetPools.
    template <typename TAssetFxImpactTable>
    size_t ExportFxImpactTablePool(Zone& Z, AssetDumpingContext& Context)
    {
        size_t Written = 0;
        auto* GlobalPools = GameGlobalAssetPools::GetGlobalPoolsForGame(Z.m_game_id);
        for (auto* Info : Z.m_pools.template PoolAssets<TAssetFxImpactTable>())
        {
            const auto* T = Info ? Info->Asset() : nullptr;
            if (!T)
                continue;
            // Keyed on the POOL's registration name (Info->m_name), not the struct's own "name" field:
            // an unresolved cross-zone reference reads back with an empty struct.name even though the
            // pool name is valid.
            const std::string Name = SafeCString(Info->m_name.c_str());
            if (Name.empty())
                continue;
            if ((!T->table || SafeCString(T->name).empty()) && GlobalPools)
            {
                if (auto* Resolved = GlobalPools->GetAsset<TAssetFxImpactTable>(Info->m_name))
                    if (Resolved->Asset() && Resolved->Asset()->table && !SafeCString(Resolved->Asset()->name).empty())
                        T = Resolved->Asset();
            }
            if (!T->table || SafeCString(T->name).empty())
                continue;
            const auto AssetFile = Context.OpenAssetFile("fx_impact_table/" + Name + ".json");
            if (!AssetFile) continue;
            auto& Out = *AssetFile;

            constexpr size_t NonfleshCount = std::extent_v<std::remove_reference_t<decltype(T->table->nonflesh)>>;
            constexpr size_t FleshCount = std::extent_v<std::remove_reference_t<decltype(T->table->flesh)>>;

            Out << "{\n";
            Out << "  \"name\": " << NameJson(T->name) << ",\n";
            Out << "  \"nonflesh\": [";
            for (size_t i = 0; i < NonfleshCount; i++)
                Out << (i ? "," : "") << NameJson(T->table->nonflesh[i] ? T->table->nonflesh[i]->name : nullptr);
            Out << "],\n";
            Out << "  \"flesh\": [";
            for (size_t i = 0; i < FleshCount; i++)
                Out << (i ? "," : "") << NameJson(T->table->flesh[i] ? T->table->flesh[i]->name : nullptr);
            Out << "]\n";
            Out << "}\n";
            Written++;
        }
        return Written;
    }

    // keyvaluepairs (T6-only). Fully flat hash-keyed value table, no asset pointers, no recursion.
    // OAT's loader handles it completely.
    size_t ExportKeyValuePairsPoolT6(Zone& Z, AssetDumpingContext& Context)
    {
        size_t Written = 0;
        for (auto* Info : Z.m_pools.PoolAssets<T6::AssetKeyValuePairs>())
        {
            const auto* K = Info ? Info->Asset() : nullptr;
            const std::string Name = K ? SafeCString(K->name) : "";
            if (Name.empty())
                continue;
            const auto AssetFile = Context.OpenAssetFile("keyvaluepairs/" + Name + ".json");
            if (!AssetFile) continue;
            auto& Out = *AssetFile;

            Out << "{\n";
            Out << "  \"name\": " << NameJson(K->name) << ",\n";
            Out << "  \"numVariables\": " << K->numVariables << ",\n";
            Out << "  \"keyValuePairs\": [";
            for (unsigned i = 0; i < K->numVariables; i++)
            {
                const auto& Kv = K->keyValuePairs[i];
                // Only the hash is stored on disk, not the original key string - nothing lost by
                // this export, there is no key name to recover.
                Out << (i ? "," : "") << "{ \"keyHash\": " << Kv.keyHash
                    << ", \"namespaceHash\": " << Kv.namespaceHash
                    << ", \"value\": " << NameJson(Kv.value) << " }";
            }
            Out << "]\n";
            Out << "}\n";
            Written++;
        }
        return Written;
    }

    // ddl. The only recursion in the whole type is ddlDef_t::next - a singly-linked list of format
    // versions with no separate length field, walked until null. structList/enumList are flat
    // arrays hanging off each version, never recursing further. T6 adds a hashTable accelerator to
    // both ddlStructDef_t/ddlEnumDef_t (lookup-only, redundant with members/index - not written).
    template <typename TAssetDdl>
    size_t ExportDdlPool(Zone& Z, AssetDumpingContext& Context)
    {
        size_t Written = 0;
        for (auto* Info : Z.m_pools.template PoolAssets<TAssetDdl>())
        {
            const auto* R = Info ? Info->Asset() : nullptr;
            const std::string Name = R ? SafeCString(R->name) : "";
            if (Name.empty())
                continue;
            const auto AssetFile = Context.OpenAssetFile("ddl/" + Name + ".json");
            if (!AssetFile) continue;
            auto& Out = *AssetFile;

            Out << "{\n";
            Out << "  \"name\": " << NameJson(R->name) << ",\n";
            Out << "  \"versions\": [\n";
            int VersionIdx = 0;
            for (const auto* D = R->ddlDef; D != nullptr && VersionIdx < 64; D = D->next, VersionIdx++)
            {
                Out << "    {\n";
                Out << "      \"version\": " << D->version << ",\n";
                Out << "      \"size\": " << D->size << ",\n";
                Out << "      \"structCount\": " << D->structCount << ",\n";
                Out << "      \"structs\": [\n";
                for (int s = 0; s < D->structCount; s++)
                {
                    const auto& Sd = D->structList[s];
                    Out << "        { \"name\": " << NameJson(Sd.name) << ", \"size\": " << Sd.size
                        << ", \"memberCount\": " << Sd.memberCount << ", \"members\": [";
                    for (int m = 0; m < Sd.memberCount; m++)
                    {
                        const auto& Md = Sd.members[m];
                        Out << (m ? "," : "") << "{ \"name\": " << NameJson(Md.name) << ", \"size\": " << Md.size
                            << ", \"offset\": " << Md.offset << ", \"type\": " << Md.type
                            << ", \"externalIndex\": " << Md.externalIndex
                            << ", \"serverDelta\": " << Md.serverDelta << ", \"clientDelta\": " << Md.clientDelta
                            << ", \"arraySize\": " << Md.arraySize << ", \"enumIndex\": " << Md.enumIndex
                            << ", \"permission\": " << Md.permission;
                        if constexpr (requires { Md.min; })
                            Out << ", \"min\": " << Md.min << ", \"max\": " << Md.max;
                        else
                            Out << ", \"rangeLimit\": " << Md.rangeLimit;
                        Out << " }";
                    }
                    Out << "] }" << (s + 1 < D->structCount ? "," : "") << "\n";
                }
                Out << "      ],\n";
                Out << "      \"enumCount\": " << D->enumCount << ",\n";
                Out << "      \"enums\": [\n";
                for (int e = 0; e < D->enumCount; e++)
                {
                    const auto& Ed = D->enumList[e];
                    Out << "        { \"name\": " << NameJson(Ed.name) << ", \"memberCount\": " << Ed.memberCount
                        << ", \"members\": [";
                    for (int m = 0; m < Ed.memberCount; m++)
                        Out << (m ? "," : "") << NameJson(Ed.members ? Ed.members[m] : nullptr);
                    Out << "] }" << (e + 1 < D->enumCount ? "," : "") << "\n";
                }
                Out << "      ]\n";
                Out << "    }" << (D->next ? "," : "") << "\n";
            }
            Out << "  ]\n";
            Out << "}\n";
            Written++;
        }
        return Written;
    }

    // T5::SoundFile is a discriminated union (T5_Assets.h:1991-1996): u.loadSnd if type==SAT_LOADED,
    // else u.streamSnd - a two-way check, not a 4-way switch over snd_alias_type_t (SAT_PRIMED is
    // represented INSIDE StreamedSound via a non-null primeSnd, not its own union arm). Same
    // struct/branch as ExtractT5ZoneSounds; this writes just a name/filename reference, not the audio
    // payload (that's the _sounds/ output next to this JSON).
    std::string SoundFileRefJson(const T5::SoundFile* SF)
    {
        if (!SF)
            return "null";
        if (SF->type == T5::SAT_LOADED && SF->u.loadSnd)
            return "{ \"kind\": \"loaded\", \"name\": " + NameJson(SF->u.loadSnd->name) + " }";
        if (SF->u.streamSnd)
        {
            const bool Primed = SF->u.streamSnd->primeSnd != nullptr;
            return "{ \"kind\": \"streamed\", \"filename\": " + NameJson(SF->u.streamSnd->filename)
                + ", \"primed\": " + (Primed ? "true" : "false") + " }";
        }
        return "null";
    }

    // sound_patch. T6's version is fully flat (name + a plain uint array) and written completely.
    // T5's also has a "files" array of SoundFile (see SoundFileRefJson for the union logic).
    template <typename TAssetSoundPatch>
    size_t ExportSoundPatchPool(Zone& Z, AssetDumpingContext& Context)
    {
        size_t Written = 0;
        for (auto* Info : Z.m_pools.template PoolAssets<TAssetSoundPatch>())
        {
            const auto* P = Info ? Info->Asset() : nullptr;
            const std::string Name = P ? SafeCString(P->name) : "";
            if (Name.empty())
                continue;
            const auto AssetFile = Context.OpenAssetFile("sound_patch/" + Name + ".json");
            if (!AssetFile) continue;
            auto& Out = *AssetFile;

            Out << "{\n";
            Out << "  \"name\": " << NameJson(P->name) << ",\n";
            Out << "  \"elementCount\": " << P->elementCount << ",\n";
            Out << "  \"elements\": [";
            for (unsigned i = 0; i < P->elementCount; i++)
                Out << (i ? "," : "") << (P->elements ? P->elements[i] : 0u);
            Out << "]";
            if constexpr (requires { P->fileCount; })
            {
                Out << ",\n  \"fileCount\": " << P->fileCount << ",\n  \"files\": [";
                for (unsigned i = 0; i < P->fileCount; i++)
                    Out << (i ? "," : "") << SoundFileRefJson(P->files ? &P->files[i] : nullptr);
                Out << "]\n";
            }
            else
                Out << "\n";
            Out << "}\n";
            Written++;
        }
        return Written;
    }

    // sound_bank (T5). Fully flat except the alias table's SoundFile union (see SoundFileRefJson).
    // aliasIndex (a hash-lookup accelerator, sized by convention not a stored count) is skipped -
    // every alias it indexes is already written in full via the alias/head arrays below.
    size_t ExportSoundBankPoolT5(Zone& Z, AssetDumpingContext& Context)
    {
        size_t Written = 0;
        for (auto* Info : Z.m_pools.PoolAssets<T5::AssetSoundBank>())
        {
            const auto* B = Info ? Info->Asset() : nullptr;
            const std::string Name = B ? SafeCString(B->name) : "";
            if (Name.empty())
                continue;
            const auto AssetFile = Context.OpenAssetFile("sound_bank/" + Name + ".json");
            if (!AssetFile) continue;
            auto& Out = *AssetFile;

            Out << "{\n";
            Out << "  \"name\": " << NameJson(B->name) << ",\n";
            Out << "  \"aliasCount\": " << B->aliasCount << ",\n";
            Out << "  \"alias\": [\n";
            for (unsigned i = 0; i < B->aliasCount; i++)
            {
                const auto& L = B->alias[i];
                Out << "    { \"name\": " << NameJson(L.name) << ", \"id\": " << L.id
                    << ", \"count\": " << L.count << ", \"entries\": [";
                for (int j = 0; j < L.count; j++)
                {
                    const auto& A = L.head[j];
                    Out << (j ? "," : "") << "{ \"name\": " << NameJson(A.name)
                        << ", \"subtitle\": " << NameJson(A.subtitle)
                        << ", \"secondaryname\": " << NameJson(A.secondaryname)
                        << ", \"soundFile\": " << SoundFileRefJson(A.soundFile)
                        << ", \"flags\": " << A.flags << ", \"duck\": " << A.duck
                        << ", \"contextType\": " << A.contextType << ", \"contextValue\": " << A.contextValue
                        << ", \"fluxTime\": " << A.fluxTime << ", \"startDelay\": " << A.startDelay
                        << ", \"reverbSend\": " << A.reverbSend << ", \"centerSend\": " << A.centerSend
                        << ", \"volMin\": " << A.volMin << ", \"volMax\": " << A.volMax
                        << ", \"teamVolMod\": " << A.teamVolMod
                        << ", \"pitchMin\": " << A.pitchMin << ", \"pitchMax\": " << A.pitchMax
                        << ", \"teamPitchMod\": " << A.teamPitchMod
                        << ", \"distMin\": " << A.distMin << ", \"distMax\": " << A.distMax
                        << ", \"distReverbMax\": " << A.distReverbMax
                        << ", \"envelopMin\": " << A.envelopMin << ", \"envelopMax\": " << A.envelopMax
                        << ", \"envelopPercentage\": " << A.envelopPercentage
                        << ", \"minPriorityThreshold\": " << (int)A.minPriorityThreshold
                        << ", \"maxPriorityThreshold\": " << (int)A.maxPriorityThreshold
                        << ", \"probability\": " << (int)A.probability
                        << ", \"occlusionLevel\": " << (int)A.occlusionLevel
                        << ", \"occlusionWetDry\": " << (int)A.occlusionWetDry
                        << ", \"minPriority\": " << (int)A.minPriority << ", \"maxPriority\": " << (int)A.maxPriority
                        << ", \"pan\": " << (int)A.pan
                        << ", \"dryCurve\": " << (int)A.dryCurve << ", \"wetCurve\": " << (int)A.wetCurve
                        << ", \"dryMinCurve\": " << (int)A.dryMinCurve << ", \"wetMinCurve\": " << (int)A.wetMinCurve
                        << ", \"limitCount\": " << (int)A.limitCount
                        << ", \"entityLimitCount\": " << (int)A.entityLimitCount
                        << ", \"snapshotGroup\": " << (int)A.snapshotGroup << " }";
                }
                Out << "] }" << (i + 1 < B->aliasCount ? "," : "") << "\n";
            }
            Out << "  ],\n";
            Out << "  \"radverbCount\": " << B->radverbCount << ",\n";
            Out << "  \"radverbs\": [";
            for (unsigned i = 0; i < B->radverbCount; i++)
            {
                const auto& R = B->radverbs[i];
                Out << (i ? "," : "") << "{ \"name\": \"" << JsonEscape(SafeCString(R.name, 32)) << "\""
                    << ", \"smoothing\": " << R.smoothing << ", \"earlyTime\": " << R.earlyTime
                    << ", \"lateTime\": " << R.lateTime << ", \"earlyGain\": " << R.earlyGain
                    << ", \"lateGain\": " << R.lateGain << ", \"returnGain\": " << R.returnGain
                    << ", \"earlyLpf\": " << R.earlyLpf << ", \"lateLpf\": " << R.lateLpf
                    << ", \"inputLpf\": " << R.inputLpf << ", \"dampLpf\": " << R.dampLpf
                    << ", \"wallReflect\": " << R.wallReflect << ", \"dryGain\": " << R.dryGain
                    << ", \"earlySize\": " << R.earlySize << ", \"lateSize\": " << R.lateSize
                    << ", \"diffusion\": " << R.diffusion << " }";
            }
            Out << "],\n";
            Out << "  \"snapshotCount\": " << B->snapshotCount << ",\n";
            Out << "  \"snapshots\": [";
            for (unsigned i = 0; i < B->snapshotCount; i++)
            {
                const auto& S = B->snapshots[i];
                Out << (i ? "," : "") << "{ \"name\": \"" << JsonEscape(SafeCString(S.name, 32)) << "\""
                    << ", \"occlusionName\": \"" << JsonEscape(SafeCString(S.occlusionName, 32)) << "\""
                    << ", \"fadeIn\": " << S.fadeIn << ", \"fadeOut\": " << S.fadeOut
                    << ", \"distance\": " << S.distance << " }";
            }
            Out << "]\n";
            Out << "}\n";
            Written++;
        }
        return Written;
    }

    // sound_bank (T6). Bitfield flags (SndAliasFlags, T6_Assets.h:6321-6353) decoded field-by-field,
    // not dumped as raw uint32s. Skipped as not structurally meaningful: aliasIndex (hash accelerator,
    // redundant with alias/head below), streamAssetBank/loadAssetBank (raw checksum/padding + an OS
    // file handle), loadedAssets.data (raw audio, extracted separately by SndBankDumperT6), and
    // state/streamRequestId/pendingIo/ioError/runtimeAssetLoad (runtime-only load state).
    size_t ExportSoundBankPoolT6(Zone& Z, AssetDumpingContext& Context)
    {
        size_t Written = 0;
        for (auto* Info : Z.m_pools.PoolAssets<T6::AssetSoundBank>())
        {
            const auto* B = Info ? Info->Asset() : nullptr;
            const std::string Name = B ? SafeCString(B->name) : "";
            if (Name.empty())
                continue;
            const auto AssetFile = Context.OpenAssetFile("sound_bank/" + Name + ".json");
            if (!AssetFile) continue;
            auto& Out = *AssetFile;

            Out << "{\n";
            Out << "  \"name\": " << NameJson(B->name) << ",\n";
            Out << "  \"aliasCount\": " << B->aliasCount << ",\n";
            Out << "  \"alias\": [\n";
            for (unsigned i = 0; i < B->aliasCount; i++)
            {
                const auto& L = B->alias[i];
                Out << "    { \"name\": " << NameJson(L.name) << ", \"id\": " << L.id
                    << ", \"count\": " << L.count << ", \"entries\": [";
                for (int j = 0; j < L.count; j++)
                {
                    const auto& A = L.head[j];
                    Out << (j ? "," : "") << "{ \"name\": " << NameJson(A.name)
                        << ", \"subtitle\": " << NameJson(A.subtitle)
                        << ", \"secondaryName\": " << NameJson(A.secondaryName)
                        << ", \"assetId\": " << A.assetId << ", \"assetFileName\": " << NameJson(A.assetFileName)
                        << ", \"flags\": { "
                        << "\"looping\": " << A.flags.looping << ", \"panType\": " << A.flags.panType
                        << ", \"distanceLpf\": " << A.flags.distanceLpf << ", \"doppler\": " << A.flags.doppler
                        << ", \"isBig\": " << A.flags.isBig << ", \"pauseable\": " << A.flags.pauseable
                        << ", \"isMusic\": " << A.flags.isMusic << ", \"stopOnEntDeath\": " << A.flags.stopOnEntDeath
                        << ", \"timescale\": " << A.flags.timescale << ", \"voiceLimit\": " << A.flags.voiceLimit
                        << ", \"ignoreMaxDist\": " << A.flags.ignoreMaxDist << ", \"busType\": " << A.flags.busType
                        << ", \"loadType\": " << A.flags.loadType << ", \"volumeGroup\": " << A.flags.volumeGroup
                        << ", \"fluxType\": " << A.flags.fluxType << ", \"limitType\": " << A.flags.limitType
                        << ", \"entityLimitType\": " << A.flags.entityLimitType
                        << ", \"randomizeType\": " << A.flags.randomizeType
                        << ", \"neverPlayTwice\": " << A.flags.neverPlayTwice
                        << ", \"volumeFalloffCurve\": " << A.flags.volumeFalloffCurve
                        << ", \"reverbFalloffCurve\": " << A.flags.reverbFalloffCurve
                        << ", \"volumeMinFalloffCurve\": " << A.flags.volumeMinFalloffCurve
                        << ", \"reverbMinFalloffCurve\": " << A.flags.reverbMinFalloffCurve
                        << ", \"isCinematic\": " << A.flags.isCinematic << " }"
                        << ", \"duck\": " << A.duck << ", \"contextType\": " << A.contextType
                        << ", \"contextValue\": " << A.contextValue << ", \"stopOnPlay\": " << A.stopOnPlay
                        << ", \"futzPatch\": " << A.futzPatch
                        << ", \"fluxTime\": " << A.fluxTime << ", \"startDelay\": " << A.startDelay
                        << ", \"reverbSend\": " << A.reverbSend << ", \"centerSend\": " << A.centerSend
                        << ", \"volMin\": " << A.volMin << ", \"volMax\": " << A.volMax
                        << ", \"pitchMin\": " << A.pitchMin << ", \"pitchMax\": " << A.pitchMax
                        << ", \"distMin\": " << A.distMin << ", \"distMax\": " << A.distMax
                        << ", \"distReverbMax\": " << A.distReverbMax
                        << ", \"envelopMin\": " << A.envelopMin << ", \"envelopMax\": " << A.envelopMax
                        << ", \"envelopPercentage\": " << A.envelopPercentage
                        << ", \"fadeIn\": " << A.fadeIn << ", \"fadeOut\": " << A.fadeOut
                        << ", \"dopplerScale\": " << A.dopplerScale
                        << ", \"minPriorityThreshold\": " << (int)A.minPriorityThreshold
                        << ", \"maxPriorityThreshold\": " << (int)A.maxPriorityThreshold
                        << ", \"probability\": " << (int)A.probability
                        << ", \"occlusionLevel\": " << (int)A.occlusionLevel
                        << ", \"minPriority\": " << (int)A.minPriority << ", \"maxPriority\": " << (int)A.maxPriority
                        << ", \"pan\": " << (int)A.pan << ", \"limitCount\": " << (int)A.limitCount
                        << ", \"entityLimitCount\": " << (int)A.entityLimitCount
                        << ", \"duckGroup\": " << (int)A.duckGroup << " }";
                }
                Out << "] }" << (i + 1 < B->aliasCount ? "," : "") << "\n";
            }
            Out << "  ],\n";
            Out << "  \"radverbCount\": " << B->radverbCount << ",\n";
            Out << "  \"radverbs\": [";
            for (unsigned i = 0; i < B->radverbCount; i++)
            {
                const auto& R = B->radverbs[i];
                Out << (i ? "," : "") << "{ \"name\": \"" << JsonEscape(SafeCString(R.name, 32)) << "\""
                    << ", \"smoothing\": " << R.smoothing << ", \"earlyTime\": " << R.earlyTime
                    << ", \"lateTime\": " << R.lateTime << ", \"earlyGain\": " << R.earlyGain
                    << ", \"lateGain\": " << R.lateGain << ", \"returnGain\": " << R.returnGain
                    << ", \"earlyLpf\": " << R.earlyLpf << ", \"lateLpf\": " << R.lateLpf
                    << ", \"inputLpf\": " << R.inputLpf << ", \"dampLpf\": " << R.dampLpf
                    << ", \"wallReflect\": " << R.wallReflect << ", \"dryGain\": " << R.dryGain
                    << ", \"earlySize\": " << R.earlySize << ", \"lateSize\": " << R.lateSize
                    << ", \"diffusion\": " << R.diffusion << ", \"returnHighpass\": " << R.returnHighpass << " }";
            }
            Out << "],\n";
            Out << "  \"duckCount\": " << B->duckCount << ",\n";
            Out << "  \"ducks\": [";
            for (unsigned i = 0; i < B->duckCount; i++)
            {
                const auto& D = B->ducks[i];
                Out << (i ? "," : "") << "{ \"name\": \"" << JsonEscape(SafeCString(D.name, 32)) << "\""
                    << ", \"fadeIn\": " << D.fadeIn << ", \"fadeOut\": " << D.fadeOut
                    << ", \"startDelay\": " << D.startDelay << ", \"distance\": " << D.distance
                    << ", \"length\": " << D.length << ", \"fadeInCurve\": " << D.fadeInCurve
                    << ", \"fadeOutCurve\": " << D.fadeOutCurve << ", \"updateWhilePaused\": " << D.updateWhilePaused
                    << ", \"attenuation\": [";
                for (int k = 0; k < 32; k++)
                    Out << (k ? "," : "") << (D.attenuation ? D.attenuation[k] : 0.0f);
                Out << "], \"filter\": [";
                for (int k = 0; k < 32; k++)
                    Out << (k ? "," : "") << (D.filter ? D.filter[k] : 0.0f);
                Out << "] }";
            }
            Out << "],\n";
            Out << "  \"scriptIdLookupCount\": " << B->scriptIdLookupCount << ",\n";
            Out << "  \"scriptIdLookups\": [";
            for (unsigned i = 0; i < B->scriptIdLookupCount; i++)
                Out << (i ? "," : "") << "{ \"scriptId\": " << B->scriptIdLookups[i].scriptId
                    << ", \"aliasId\": " << B->scriptIdLookups[i].aliasId << " }";
            Out << "]\n";
            Out << "}\n";
            Written++;
        }
        return Written;
    }

    // windowDef_t - shared layout/color struct embedded in both menuDef_t and itemDef_s. Identical
    // shape both games except foreColor/backColor/borderColor/outlineColor: plain float[4] in T5,
    // vec4_t in T6 - WriteFloatVec already handles both via the same if-constexpr dispatch used
    // everywhere else in this file.
    template <typename TWindowDef>
    void WriteWindowDef(std::ostream& Out, const TWindowDef& W)
    {
        Out << "{ \"name\": " << NameJson(W.name)
            << ", \"rect\": { \"x\": " << W.rect.x << ", \"y\": " << W.rect.y << ", \"w\": " << W.rect.w
            << ", \"h\": " << W.rect.h << ", \"horzAlign\": " << W.rect.horzAlign
            << ", \"vertAlign\": " << W.rect.vertAlign << " }"
            << ", \"rectClient\": { \"x\": " << W.rectClient.x << ", \"y\": " << W.rectClient.y
            << ", \"w\": " << W.rectClient.w << ", \"h\": " << W.rectClient.h << " }"
            << ", \"group\": " << NameJson(W.group)
            << ", \"style\": " << (int)W.style << ", \"border\": " << (int)W.border
            << ", \"modal\": " << (int)W.modal << ", \"frameSides\": " << (int)W.frameSides
            << ", \"frameTexSize\": " << W.frameTexSize << ", \"frameSize\": " << W.frameSize
            << ", \"ownerDraw\": " << W.ownerDraw << ", \"ownerDrawFlags\": " << W.ownerDrawFlags
            << ", \"borderSize\": " << W.borderSize << ", \"staticFlags\": " << W.staticFlags
            << ", \"foreColor\": "; WriteFloatVec<4>(Out, W.foreColor);
        Out << ", \"backColor\": "; WriteFloatVec<4>(Out, W.backColor);
        Out << ", \"borderColor\": "; WriteFloatVec<4>(Out, W.borderColor);
        Out << ", \"outlineColor\": "; WriteFloatVec<4>(Out, W.outlineColor);
        Out << ", \"rotation\": " << W.rotation
            << ", \"background\": " << NameJson(W.background ? W.background->info.name : nullptr)
            << " }";
    }

    // menu_list - trivial: name + an array of menuDef_t* pointers, each referenced by its own
    // window.name (menuDef_t is itself a separately-registered top-level asset, dumped in full by
    // ExportMenuDefPool below - this file just lists which menus the list points at).
    template <typename TAssetMenuList>
    size_t ExportMenuListPool(Zone& Z, AssetDumpingContext& Context)
    {
        size_t Written = 0;
        for (auto* Info : Z.m_pools.template PoolAssets<TAssetMenuList>())
        {
            const auto* M = Info ? Info->Asset() : nullptr;
            const std::string Name = M ? SafeCString(M->name) : "";
            if (Name.empty())
                continue;
            const auto AssetFile = Context.OpenAssetFile("menu_list/" + Name + ".json");
            if (!AssetFile) continue;
            auto& Out = *AssetFile;

            Out << "{\n  \"name\": " << NameJson(M->name) << ",\n  \"menuCount\": " << M->menuCount
                << ",\n  \"menus\": [";
            for (int i = 0; i < M->menuCount; i++)
            {
                const auto* Menu = M->menus ? M->menus[i] : nullptr;
                Out << (i ? "," : "") << NameJson(Menu ? Menu->window.name : nullptr);
            }
            Out << "]\n}\n";
            Written++;
        }
        return Written;
    }

    // menu_def (menuDef_t): full flat metadata + windowDef_t + itemDef_s array, deliberately skipping
    // three hard-to-serialize things rather than guessing:
    //  - itemDef_s::typeData - a 3-level discriminated union chain, each terminal struct its own
    //    multi-field type not covered by this pass.
    //  - visibleExp/rectXExp/rectYExp/forecolorAExp - compiled RPN bytecode trees, real "code" not
    //    asset data.
    //  - onEvent/onKey - self-referential linked lists of the same kind of compiled command data.
    // itemDef_s::parent (a back-pointer cycling with this menu's own items[]) is reported as a bool
    // only, never re-dumped, to avoid an infinite structure.
    template <typename TAssetMenu>
    size_t ExportMenuDefPool(Zone& Z, AssetDumpingContext& Context)
    {
        size_t Written = 0;
        for (auto* Info : Z.m_pools.template PoolAssets<TAssetMenu>())
        {
            const auto* D = Info ? Info->Asset() : nullptr;
            const std::string Name = D ? SafeCString(D->window.name) : "";
            if (Name.empty())
                continue;
            const auto AssetFile = Context.OpenAssetFile("menu_def/" + Name + ".json");
            if (!AssetFile) continue;
            auto& Out = *AssetFile;

            Out << "{\n  \"window\": "; WriteWindowDef(Out, D->window); Out << ",\n";
            Out << "  \"font\": " << NameJson(D->font) << ",\n";
            Out << "  \"fullScreen\": " << D->fullScreen << ", \"itemCount\": " << D->itemCount
                << ", \"priority\": " << D->priority << ",\n";
            Out << "  \"fadeClamp\": " << D->fadeClamp << ", \"fadeAmount\": " << D->fadeAmount
                << ", \"fadeInAmount\": " << D->fadeInAmount << ", \"blurRadius\": " << D->blurRadius << ",\n";
            Out << "  \"allowedBinding\": " << NameJson(D->allowedBinding) << ",\n";
            Out << "  \"soundName\": " << NameJson(D->soundName) << ",\n";
            Out << "  \"imageTrack\": " << D->imageTrack << ", \"control\": " << D->control << ",\n";
            Out << "  \"showBits\": " << D->showBits << ", \"hideBits\": " << D->hideBits << ",\n";
            Out << "  \"focusColor\": "; WriteFloatVec<4>(Out, D->focusColor);
            Out << ",\n  \"disableColor\": "; WriteFloatVec<4>(Out, D->disableColor);
            Out << ",\n  \"items\": [\n";
            for (int i = 0; i < D->itemCount; i++)
            {
                const auto* It = D->items ? D->items[i] : nullptr;
                if (!It)
                {
                    Out << "    null" << (i + 1 < D->itemCount ? "," : "") << "\n";
                    continue;
                }
                Out << "    { \"window\": "; WriteWindowDef(Out, It->window); Out << ",\n";
                Out << "      \"type\": " << It->type << ", \"dataType\": " << It->dataType
                    << ", \"imageTrack\": " << It->imageTrack << ",\n";
                Out << "      \"dvar\": " << NameJson(It->dvar) << ",\n";
                if constexpr (requires { It->profileVar; })
                    Out << "      \"profileVar\": " << NameJson(It->profileVar) << ",\n";
                Out << "      \"dvarTest\": " << NameJson(It->dvarTest) << ",\n";
                Out << "      \"enableDvar\": " << NameJson(It->enableDvar) << ",\n";
                Out << "      \"dvarFlags\": " << It->dvarFlags << ",\n";
                Out << "      \"hasParent\": " << (It->parent ? "true" : "false") << ",\n";
                Out << "      \"showBits\": " << It->showBits << ", \"hideBits\": " << It->hideBits << "\n";
                Out << "    }" << (i + 1 < D->itemCount ? "," : "") << "\n";
            }
            Out << "  ]\n}\n";
            Written++;
        }
        return Written;
    }

    // gameworld_sp/gameworld_mp - name + one PathData. Scoped to safe "constant" node data + the
    // decoded nodeTree kd-tree; runtime-only fields and opaque byte blobs are skipped.
    template <typename TPathData>
    void WritePathData(std::ostream& Out, const TPathData& Path)
    {
        Out << "  \"path\": {\n";
        Out << "    \"nodeCount\": " << Path.nodeCount << ",\n";
        if constexpr (requires { Path.originalNodeCount; })
            Out << "    \"originalNodeCount\": " << Path.originalNodeCount << ",\n";
        if constexpr (requires { Path.chainNodeCount; })
            Out << "    \"chainNodeCount\": " << Path.chainNodeCount << ",\n";
        Out << "    \"visBytes\": " << Path.visBytes << ",\n";
        if constexpr (requires { Path.smoothBytes; })
            Out << "    \"smoothBytes\": " << Path.smoothBytes << ",\n";
        Out << "    \"nodeTreeCount\": " << Path.nodeTreeCount << ",\n";
        Out << "    \"nodes\": [\n";
        for (unsigned i = 0; i < Path.nodeCount; i++)
        {
            const auto& C = Path.nodes[i].constant;
            Out << "      { \"type\": " << (int)C.type << ", \"spawnflags\": " << (unsigned)C.spawnflags
                << ", \"origin\": "; WriteFloatVec<3>(Out, C.vOrigin); Out << ", \"fAngle\": " << C.fAngle
                << ", \"fRadius\": " << C.fRadius << ", \"totalLinkCount\": " << C.totalLinkCount
                << ", \"links\": [";
            for (unsigned l = 0; l < C.totalLinkCount; l++)
                Out << (l ? "," : "") << "{ \"nodeNum\": " << C.Links[l].nodeNum
                    << ", \"fDist\": " << C.Links[l].fDist << " }";
            Out << "] }" << (i + 1 < Path.nodeCount ? "," : "") << "\n";
        }
        Out << "    ],\n";
        Out << "    \"nodeTree\": [\n";
        const auto* TreeBase = Path.nodeTree;
        const auto* TreeEnd = Path.nodeTree ? Path.nodeTree + Path.nodeTreeCount : nullptr;
        for (int i = 0; TreeBase && i < Path.nodeTreeCount; i++)
        {
            const auto& N = TreeBase[i];
            Out << "      { \"axis\": " << N.axis << ", \"dist\": " << N.dist;
            if ((const void*)N.u.child[0] < (const void*)0x10000)
            {
                // Leaf: u.s.nodeCount/nodes is the active member, not child[0]/[1].
                Out << ", \"leaf\": true, \"nodeCount\": " << N.u.s.nodeCount << ", \"nodes\": [";
                for (int k = 0; k < N.u.s.nodeCount; k++)
                    Out << (k ? "," : "") << (N.u.s.nodes ? N.u.s.nodes[k] : (uint16_t)0);
                Out << "]";
            }
            else
            {
                // Internal: report each child as its own index back into this same flat nodeTree
                // array (bounds-checked, not dereferenced) rather than an unverified raw pointer.
                const bool In0 = N.u.child[0] >= TreeBase && N.u.child[0] < TreeEnd;
                const bool In1 = N.u.child[1] >= TreeBase && N.u.child[1] < TreeEnd;
                Out << ", \"leaf\": false, \"child0\": " << (In0 ? (int)(N.u.child[0] - TreeBase) : -1)
                    << ", \"child1\": " << (In1 ? (int)(N.u.child[1] - TreeBase) : -1);
            }
            Out << " }" << (i + 1 < Path.nodeTreeCount ? "," : "") << "\n";
        }
        Out << "    ]\n";
        Out << "  }\n";
    }

    template <typename TAssetGameWorld>
    size_t ExportGameWorldPool(Zone& Z, AssetDumpingContext& Context, const char* SubDir)
    {
        size_t Written = 0;
        for (auto* Info : Z.m_pools.template PoolAssets<TAssetGameWorld>())
        {
            const auto* G = Info ? Info->Asset() : nullptr;
            const std::string Name = G ? SafeCString(G->name) : "";
            if (Name.empty())
                continue;
            const auto AssetFile = Context.OpenAssetFile(std::string(SubDir) + "/" + Name + ".json");
            if (!AssetFile) continue;
            auto& Out = *AssetFile;

            Out << "{\n";
            Out << "  \"name\": " << NameJson(G->name) << ",\n";
            WritePathData(Out, G->path);
            Out << "}\n";
            Written++;
        }
        return Written;
    }
}

namespace
{
    // OAT's filesystem output, counting the files it opens
    class CountingOutputPath final : public IOutputPath
    {
    public:
        CountingOutputPath(const std::filesystem::path& Path, size_t& Files) : Inner(Path), Files(Files) {}
        std::unique_ptr<std::ostream> Open(const std::string& FileName) override
        {
            auto S = Inner.Open(FileName);
            if (S) Files++;
            return S;
        }

    private:
        OutputPathFilesystem Inner;
        size_t& Files;
    };
}

struct OatZoneSession::Impl
{
    // Companions load BEFORE the requested zone and stay alive with it (see LoadCompanionZones):
    // every Zone gets the same load priority and GlobalAssetPool::LinkAsset keeps the FIRST-linked
    // entry on a tie, so companions must load first or the zone's own unresolved stub would win.
    std::vector<std::unique_ptr<Zone>> Companions;
    std::unique_ptr<Zone> Z;
    GameId Game = GameId::IW3;
    const IGame* GameInfo = nullptr;
    const IObjLoader* ObjLoader = nullptr;
    SharedSearchPaths Paths;
    std::string FastFile, ZoneDir;
};

OatZoneSession::OatZoneSession() : P(std::make_unique<Impl>()) {}

OatZoneSession::~OatZoneSession()
{
    if (P && P->ObjLoader && P->Z) P->ObjLoader->UnloadContainersOfZone(*P->Z);
}

std::unique_ptr<OatZoneSession> OatZoneSession::Open(const std::string& FastFilePath, std::string& Report)
{
    std::unique_ptr<OatZoneSession> S(new OatZoneSession());
    Impl& I = *S->P;
    I.FastFile = FastFilePath;
    I.Companions = LoadCompanionZones(FastFilePath);
    I.Z = LoadZoneOrReport(FastFilePath, Report);
    if (!I.Z)
        return nullptr;
    I.Game = I.Z->m_game_id;
    I.GameInfo = IGame::GetGameById(I.Game);
    if (!I.GameInfo)
    {
        Report = "no IGame for zone";
        return nullptr;
    }
    // the game's auto search paths for this fastfile's directory, so referenced containers
    // (IPAKs, loose images, ...) are found - exactly like Unlinker.cpp does
    std::error_code Ec;
    I.ZoneDir = std::filesystem::absolute(std::filesystem::path(FastFilePath).remove_filename(), Ec).string();
    for (const auto& Sp : AutoSearchPaths::GetForGame(I.Game)->GetSearchPathsForZonePath(I.ZoneDir))
        I.Paths.RefSearchPath(Sp);
    I.ObjLoader = IObjLoader::GetObjLoaderForGame(I.Game);
    I.ObjLoader->LoadReferencedContainersForZone(I.Paths.GetSearchPath(), *I.Z);
    return S;
}

std::string OatZoneSession::GameShortName() const { return P->GameInfo->GetShortName(); }
std::string OatZoneSession::ZoneName() const { return P->Z->m_name; }
const std::string& OatZoneSession::FastFile() const { return P->FastFile; }
size_t OatZoneSession::CompanionCount() const { return P->Companions.size(); }

size_t OatZoneSession::Count(const std::string& OatTypeName) const
{
    size_t N = 0;
    for (auto It = P->Z->m_pools.begin(); It != P->Z->m_pools.end(); ++It)
    {
        if (!*It || (*It)->IsReference()) continue;
        const auto Name = P->GameInfo->GetAssetTypeName((*It)->m_type);
        if (Name && *Name == OatTypeName) N++;
    }
    return N;
}

const char* OatZoneSession::Category(const std::string& T)
{
    if (T == "xmodel") return "model";
    if (T == "xanim") return "anim";
    if (T == "image") return "image";
    if (T == "sound" || T == "loadedsound" || T == "soundbank") return "sound";
    if (T == "rawfile" || T == "scriptfile" || T == "script") return "rawfile";
    if (T == "localize") return "localize";
    return "data";
}

namespace
{
    template <typename Entry>
    void LocalizeEntries(const Zone& Z, const IGame& Game, std::vector<std::pair<std::string, std::string>>& Out)
    {
        for (auto It = Z.m_pools.begin(); It != Z.m_pools.end(); ++It)
        {
            if (!*It || (*It)->IsReference() || !(*It)->m_ptr) continue;
            const auto Name = Game.GetAssetTypeName((*It)->m_type);
            if (!Name || std::string(*Name) != "localize") continue;
            // the key as OAT's own dumper takes it, the asset's name
            const auto* E = static_cast<const Entry*>((*It)->m_ptr);
            Out.emplace_back((*It)->m_name, E->value ? E->value : "");
        }
    }
}

void OatZoneSession::Strings(std::vector<std::pair<std::string, std::string>>& Out) const
{
    const Zone& Z = *P->Z;
    switch (P->Game)
    {
    case GameId::IW3: LocalizeEntries<IW3::LocalizeEntry>(Z, *P->GameInfo, Out); break;
    case GameId::IW4: LocalizeEntries<IW4::LocalizeEntry>(Z, *P->GameInfo, Out); break;
    case GameId::IW5: LocalizeEntries<IW5::LocalizeEntry>(Z, *P->GameInfo, Out); break;
    case GameId::T4: LocalizeEntries<T4::LocalizeEntry>(Z, *P->GameInfo, Out); break;
    case GameId::T5: LocalizeEntries<T5::LocalizeEntry>(Z, *P->GameInfo, Out); break;
    case GameId::T6: LocalizeEntries<T6::LocalizeEntry>(Z, *P->GameInfo, Out); break;
    default: break;
    }
}

void OatZoneSession::WriteZoneSource(const std::string& OutputDir, bool Gdt) const
{
    std::error_code Ec;
    const std::filesystem::path ZoneSrcDir = std::filesystem::path(OutputDir) / "zone_source";
    std::filesystem::create_directories(ZoneSrcDir, Ec);
    std::ofstream ZoneDefFile(ZoneSrcDir / (P->Z->m_name + ".zone"), std::ios::out | std::ios::binary);
    if (ZoneDefFile.is_open())
        IZoneDefWriter::GetZoneDefWriterForGame(P->Game)->WriteZoneDef(ZoneDefFile, *P->Z, Gdt, false);
}

bool OatZoneSession::Dump(const std::string& OutputDir, const std::vector<std::string>& Categories,
                          const OatDumpOptions& Options, OatDumpCounts& Counts)
{
    Impl& I = *P;
    Zone& Z = *I.Z;
    auto Wanted = [&](const char* C) { return std::find(Categories.begin(), Categories.end(), C) != Categories.end(); };

    // the writer handles the asset types of the wanted categories only
    std::vector<bool> Types(I.GameInfo->GetAssetTypeCount(), false);
    for (size_t T = 0; T < Types.size(); T++)
    {
        const auto Name = I.GameInfo->GetAssetTypeName(static_cast<asset_type_t>(T));
        Types[T] = Name && Wanted(Category(*Name));
    }
    for (auto It = Z.m_pools.begin(); It != Z.m_pools.end(); ++It)
        if (*It && (*It)->m_type < Types.size() && Types[(*It)->m_type])
            ((*It)->IsReference() ? Counts.References : Counts.Assets)++;
    ObjWriting::Configuration.AssetTypesToHandleBitfield = Types;
    ObjWriting::Configuration.ModelOutputFormat = ParseModelFormat(Options.ModelFormat);
    ObjWriting::Configuration.ImageOutputFormat = ParseImageFormat(Options.ImageFormat);

    const std::filesystem::path OutPath(OutputDir);
    std::error_code Ec;
    std::filesystem::create_directories(OutPath, Ec);
    CountingOutputPath OutputPath(OutPath, Counts.Files);
    AssetDumpingContext Context(*I.Z, OutputDir, OutputPath, I.Paths.GetSearchPath(), std::nullopt);

    // optional GDT (source_data/<zone>.gdt), same as Unlinker's --gdt
    std::ofstream GdtStream;
    if (Options.Gdt)
    {
        std::filesystem::path GdtDir = OutPath / "source_data";
        std::filesystem::create_directories(GdtDir, Ec);
        GdtStream.open(GdtDir / (Z.m_name + ".gdt"), std::ios::out | std::ios::binary);
        if (GdtStream.is_open())
        {
            auto Gdt = std::make_unique<GdtOutputStream>(GdtStream);
            Gdt->BeginStream();
            Gdt->WriteVersion(GdtVersion(I.GameInfo->GetShortName(), 1));
            Context.m_gdt = std::move(Gdt);
        }
    }

    // T6 sound files, which SndBankDumperT6 writes itself, outside the counted output
    auto CountSoundFiles = [&]() {
        size_t N = 0;
        std::error_code CountEc;
        const std::filesystem::path SoundOutDir = OutPath / "sound";
        if (std::filesystem::exists(SoundOutDir, CountEc))
            for (auto& Entry : std::filesystem::recursive_directory_iterator(
                     SoundOutDir, std::filesystem::directory_options::skip_permission_denied, CountEc))
            {
                if (CountEc) break;
                if (!Entry.is_regular_file(CountEc)) continue;
                const std::string Ext = Entry.path().extension().string();
                if (Ext == ".wav" || Ext == ".flac") N++;
            }
        return N;
    };
    const size_t SoundsBefore = Wanted("sound") && I.Game == GameId::T6 ? CountSoundFiles() : 0;

    // OAT's own dumpers - the exact call Unlinker.exe makes
    const bool Ok = IObjWriter::GetObjWriterForGame(I.Game)->DumpZone(Context);
    if (Context.m_gdt)
        Context.m_gdt->EndStream();
    if (GdtStream.is_open())
        GdtStream.close();

    // Native passes - what OAT's DumpZone cannot. T6 streamed images live in the IPAKs; T5 (BO1)
    // keeps audio as MS-ADPCM in the install's IWDs and OAT ships no dumper for it, so we decode it
    // into _sounds/. T6 needs no sound pass (SndBankDumperT6 decodes real PCM/FLAC in DumpZone): it
    // is only counted.
    if (Wanted("image") && I.Game == GameId::T6)
        Counts.StreamedImages = ExtractT6ZoneImages(Z, OutputDir, I.ZoneDir);
    if (Wanted("sound") && I.Game == GameId::T5)
    {
        const std::string InstallDir = FindInstallDir(I.FastFile);
        const std::string SoundDir = (OutPath / "_sounds").string();
        // only the sounds THIS fastfile references (loaded ones from the .ff, streamed from the IWDs)
        Counts.Sounds = ExtractT5ZoneSounds(Z, SoundDir, InstallDir);
        // opt-in: the entire install's IWD audio (every sound in the game, slow)
        if (Options.WithSounds && !InstallDir.empty())
            Counts.Sounds += ExtractIwdSounds(InstallDir, SoundDir);
    }
    else if (Wanted("sound") && I.Game == GameId::T6)
    {
        const size_t After = CountSoundFiles();
        Counts.Sounds = After;
        Counts.NewSoundFiles = After - SoundsBefore;
    }

    // Native writers, part 2: types OAT's loader understands completely but whose REGISTER_DUMPER
    // is commented out in ObjWriterT5/T6.cpp, written here through the same output tree (keeping
    // the vendored OAT submodule untouched). All of them are "data".
    if (Wanted("data") && I.Game == GameId::T5)
    {
        Counts.DestructibleDefs = ExportDestructibleDefPool<T5::AssetDestructibleDef>(Z, Context);
        Counts.XGlobals = ExportXGlobalsPool<T5::AssetXGlobals>(Z, Context);
        Counts.SndDriverGlobals = ExportSndDriverGlobalsPoolT5(Z, Context);
        Counts.Glasses = ExportGlassesPool<T5::AssetGlasses>(Z, Context);
        Counts.EmblemSets = ExportEmblemSetPool<T5::AssetEmblemSet>(Z, Context);
        Counts.PackIndex = ExportPackIndexPool(Z, Context);
        Counts.FxImpactTables = ExportFxImpactTablePool<T5::AssetImpactFx>(Z, Context);
        Counts.Ddl = ExportDdlPool<T5::AssetDDL>(Z, Context);
        Counts.SoundPatches = ExportSoundPatchPool<T5::AssetSoundPatch>(Z, Context);
        Counts.GameWorlds = ExportGameWorldPool<T5::AssetGameWorldSp>(Z, Context, "gameworld_sp")
                          + ExportGameWorldPool<T5::AssetGameWorldMp>(Z, Context, "gameworld_mp");
        Counts.SoundBanks = ExportSoundBankPoolT5(Z, Context);
        Counts.MenuLists = ExportMenuListPool<T5::AssetMenuList>(Z, Context);
        Counts.MenuDefs = ExportMenuDefPool<T5::AssetMenu>(Z, Context);
    }
    else if (Wanted("data") && I.Game == GameId::T6)
    {
        Counts.DestructibleDefs = ExportDestructibleDefPool<T6::AssetDestructibleDef>(Z, Context);
        Counts.XGlobals = ExportXGlobalsPool<T6::AssetXGlobals>(Z, Context);
        Counts.SndDriverGlobals = ExportSndDriverGlobalsPoolT6(Z, Context);
        Counts.Glasses = ExportGlassesPool<T6::AssetGlasses>(Z, Context);
        Counts.EmblemSets = ExportEmblemSetPool<T6::AssetEmblemSet>(Z, Context);
        Counts.FxImpactTables = ExportFxImpactTablePool<T6::AssetImpactFx>(Z, Context);
        Counts.KeyValuePairs = ExportKeyValuePairsPoolT6(Z, Context);
        Counts.Ddl = ExportDdlPool<T6::AssetDDL>(Z, Context);
        Counts.SoundPatches = ExportSoundPatchPool<T6::AssetSoundPatch>(Z, Context);
        Counts.GameWorlds = ExportGameWorldPool<T6::AssetGameWorldSp>(Z, Context, "gameworld_sp")
                          + ExportGameWorldPool<T6::AssetGameWorldMp>(Z, Context, "gameworld_mp");
        Counts.SoundBanks = ExportSoundBankPoolT6(Z, Context);
        Counts.MenuLists = ExportMenuListPool<T6::AssetMenuList>(Z, Context);
        Counts.MenuDefs = ExportMenuDefPool<T6::AssetMenu>(Z, Context);
    }
    return Ok;
}

namespace
{
    // OAT's output kept in memory: each file's contents by its path, stored when its stream closes
    class CaptureOutputPath final : public IOutputPath
    {
    public:
        explicit CaptureOutputPath(std::map<std::string, std::string>& Files) : Files(Files) {}
        std::unique_ptr<std::ostream> Open(const std::string& FileName) override
        {
            return std::make_unique<Capture>(Files, FileName);
        }

    private:
        class Capture final : public std::ostringstream
        {
        public:
            Capture(std::map<std::string, std::string>& Files, std::string Name)
                : std::ostringstream(std::ios::out | std::ios::binary), Files(Files), Name(std::move(Name)) {}
            ~Capture() override { Files[Name] = str(); }

        private:
            std::map<std::string, std::string>& Files;
            std::string Name;
        };
        std::map<std::string, std::string>& Files;
    };
}

bool OatZoneSession::DumpTypes(const std::vector<std::string>& Wanted, std::vector<std::pair<std::string, std::string>>& Out, size_t& Assets)
{
    Impl& I = *P;
    std::vector<bool> Types(I.GameInfo->GetAssetTypeCount(), false);
    for (size_t T = 0; T < Types.size(); T++)
    {
        const auto Name = I.GameInfo->GetAssetTypeName(static_cast<asset_type_t>(T));
        Types[T] = Name && std::find(Wanted.begin(), Wanted.end(), std::string(*Name)) != Wanted.end();
    }
    Assets = 0;
    for (auto It = I.Z->m_pools.begin(); It != I.Z->m_pools.end(); ++It)
        if (*It && !(*It)->IsReference() && (*It)->m_type < Types.size() && Types[(*It)->m_type]) Assets++;
    ObjWriting::Configuration.AssetTypesToHandleBitfield = Types;
    std::map<std::string, std::string> Files;
    CaptureOutputPath Output(Files);
    const std::string BasePath = (std::filesystem::temp_directory_path() / "graywolf_oat_weapons").string();
    bool Ok = false;
    {
        AssetDumpingContext Context(*I.Z, BasePath, Output, I.Paths.GetSearchPath(), std::nullopt);
        Ok = IObjWriter::GetObjWriterForGame(I.Game)->DumpZone(Context);
    }
    Out.assign(Files.begin(), Files.end());
    return Ok;
}

bool OatZoneSession::WeaponFiles(std::vector<std::pair<std::string, std::string>>& Out, size_t& Assets)
{
    return DumpTypes({ "weapon", "attachment", "attachmentunique", "camo" }, Out, Assets);
}

bool OatZoneSession::StringTables(std::vector<std::pair<std::string, std::string>>& Out)
{
    size_t Assets = 0;
    return DumpTypes({ "stringtable" }, Out, Assets);
}

bool OatDumpZoneInProcess(const std::string& FastFilePath, const std::string& OutputDir,
                          const OatDumpOptions& Options, std::string& Report)
{
    // the whole zone: every category of a session, plus the zone definition file - the same code
    // path Unlinker.exe drives
    auto S = OatZoneSession::Open(FastFilePath, Report);
    if (!S)
        return false;
    std::error_code Ec;
    std::filesystem::create_directories(OutputDir, Ec);
    S->WriteZoneSource(OutputDir, Options.Gdt);
    OatDumpCounts C;
    const bool Ok = S->Dump(OutputDir, { "model", "anim", "image", "sound", "rawfile", "data", "localize" }, Options, C);

    std::ostringstream Os;
    Os << "zone=" << S->ZoneName() << "\n";
    Os << "game=" << S->GameShortName() << "\n";
    Os << "total_assets=" << C.Assets + C.References << "\n";
    Os << "model_format=" << Options.ModelFormat << "\n";
    Os << "image_format=" << Options.ImageFormat << "\n";
    Os << "gdt=" << (Options.Gdt ? "yes" : "no") << "\n";
    Os << "sounds_extracted=" << C.Sounds << "\n";
    Os << "streamed_images=" << C.StreamedImages << "\n";
    Os << "destructible_defs_written=" << C.DestructibleDefs << "\n";
    Os << "xglobals_written=" << C.XGlobals << "\n";
    Os << "snd_driver_globals_written=" << C.SndDriverGlobals << "\n";
    Os << "glasses_written=" << C.Glasses << "\n";
    Os << "emblem_set_written=" << C.EmblemSets << "\n";
    Os << "pack_index_written=" << C.PackIndex << "\n";
    Os << "fx_impact_tables_written=" << C.FxImpactTables << "\n";
    Os << "keyvaluepairs_written=" << C.KeyValuePairs << "\n";
    Os << "ddl_written=" << C.Ddl << "\n";
    Os << "sound_patches_written=" << C.SoundPatches << "\n";
    Os << "gameworlds_written=" << C.GameWorlds << "\n";
    Os << "sound_banks_written=" << C.SoundBanks << "\n";
    Os << "menu_lists_written=" << C.MenuLists << "\n";
    Os << "menu_defs_written=" << C.MenuDefs << "\n";
    Os << "companion_zones=" << S->CompanionCount() << "\n";
    Os << "out=" << OutputDir << "\n";
    Os << "dump=" << (Ok ? "ok" : "failed") << "\n";
    Report = Os.str();
    return Ok;
}

bool OatListZoneInProcess(const std::string& FastFilePath, std::string& Report)
{
    std::unique_ptr<Zone> Z = LoadZoneOrReport(FastFilePath, Report);
    if (!Z)
        return false;

    const IGame* GameInfo = IGame::GetGameById(Z->m_game_id);
    std::ostringstream Os;
    Os << "zone=" << Z->m_name << "\n";
    Os << "game=" << (GameInfo ? GameInfo->GetShortName() : GameId_Names[static_cast<int>(Z->m_game_id)]) << "\n";
    Os << "total_assets=" << Z->m_pools.GetTotalAssetCount() << "\n";
    Os << "--- content ---\n";
    for (auto It = Z->m_pools.begin(); It != Z->m_pools.end(); ++It)
    {
        if (!*It)
            continue;
        const auto TypeName = GameInfo ? GameInfo->GetAssetTypeName((*It)->m_type) : std::nullopt;
        Os << (TypeName ? *TypeName : "?") << ", " << (*It)->m_name << "\n";
    }
    Report = Os.str();
    return true;
}

namespace
{
    // Formats one material's name + blend mode, e.g. " material=foo blend=alpha" ("" if Mat is null).
    // Same GfxStateBits interpretation as GameBlackOps::DescribeMaterialBlend(), reading OAT's
    // already-resolved Material* instead of a live ProcessReader pointer. T6::Material matches
    // T5::Material's field names/shapes for everything this reads, so one template covers both games.
    template <typename TMaterial>
    std::string DescribeMaterialGeneric(const TMaterial* Mat)
    {
        if (!Mat)
            return "";

        std::string Result = " material=";
        Result += (Mat->info.name ? Mat->info.name : "?");

        if (Mat->stateBitsCount > 0 && Mat->stateBitsTable)
        {
            const uint32_t Raw0 = Mat->stateBitsTable[0].loadBits.raw[0];
            const uint32_t Src = Raw0 & 0xF;
            const uint32_t Dst = (Raw0 >> 4) & 0xF;

            // GfxBlend enum: DISABLED=0, ONE=2, SRCALPHA=5, INVSRCALPHA=6
            if (Src == 2 && Dst == 0)      Result += " blend=opaque";
            else if (Src == 5 && Dst == 6) Result += " blend=alpha";
            else if (Src == 2 && Dst == 2) Result += " blend=additive";
            else Result += " blend=custom(src=" + std::to_string(Src) + ",dst=" + std::to_string(Dst) + ")";
        }

        return Result;
    }

    const char* FxElemTypeName(unsigned char ElemType)
    {
        switch (ElemType)
        {
        case 0x0: return "sprite_billboard";
        case 0x1: return "sprite_oriented";
        case 0x2: return "sprite_rotated";
        case 0x3: return "tail";
        case 0x4: return "line";
        case 0x5: return "trail";
        case 0x6: return "cloud";
        case 0x7: return "model";
        case 0x8: return "omni_light";
        case 0x9: return "spot_light";
        case 0xA: return "sound";
        case 0xB: return "decal";
        case 0xC: return "runner";
        default:  return "unknown";
        }
    }
}

namespace
{
    // T6::FxEffectDef/FxElemDef/the visuals unions match T5's field names/shapes/FxElemType values:
    // one template body for both games.
    template <typename TAssetFx>
    void DescribeFxPool(Zone& Z, std::ostream& Os, size_t& EffectCount)
    {
        for (auto* Info : Z.m_pools.template PoolAssets<TAssetFx>())
        {
            const auto* Fx = Info ? Info->Asset() : nullptr;
            if (!Fx)
                continue;
            EffectCount++;

            const int Total = Fx->elemDefCountLooping + Fx->elemDefCountOneShot + Fx->elemDefCountEmission;
            Os << "effect: " << (Fx->name ? Fx->name : "?") << " (" << Total << " elements)\n";

            if (!Fx->elemDefs)
                continue;

            for (int i = 0; i < Total; i++)
            {
                const auto& El = Fx->elemDefs[i];
                const unsigned char ElemType = (unsigned char)El.elemType;

                Os << "  elem " << i << ": " << FxElemTypeName(ElemType);

                // Only sprite/tail/line/trail/cloud (0x0-0x6) and decal (0xB) elements carry a
                // material - same rule as GameBlackOps::DescribeEffect(). visualCount<=1:
                // visuals.instance IS the material directly (a union of same-size pointers).
                // visualCount>1: visuals.array points at an array of that same union - take entry 0.
                if (ElemType <= 0x6)
                {
                    const auto* Mat = (El.visualCount <= 1)
                        ? El.visuals.instance.material
                        : (El.visuals.array ? El.visuals.array[0].material : nullptr);
                    Os << DescribeMaterialGeneric(Mat);
                }
                else if (ElemType == 0xB && El.visuals.markArray)
                {
                    Os << DescribeMaterialGeneric(El.visuals.markArray->materials[0]);
                    std::string Second = DescribeMaterialGeneric(El.visuals.markArray->materials[1]);
                    const size_t Tag = Second.find("material=");
                    if (Tag != std::string::npos)
                        Second.replace(Tag, 9, "material2=");
                    Os << Second;
                }

                Os << "\n";
            }
        }
    }
}

bool OatDescribeFxInProcess(const std::string& FastFilePath, std::string& Report)
{
    std::unique_ptr<Zone> Z = LoadZoneOrReport(FastFilePath, Report);
    if (!Z)
        return false;
    if (Z->m_game_id != GameId::T5 && Z->m_game_id != GameId::T6)
    {
        Report = "fx describe: only T5 (BO1) and T6 (BO2) supported for now";
        return false;
    }

    std::ostringstream Os;
    size_t EffectCount = 0;

    if (Z->m_game_id == GameId::T5)
        DescribeFxPool<T5::AssetFx>(*Z, Os, EffectCount);
    else
        DescribeFxPool<T6::AssetFx>(*Z, Os, EffectCount);

    Os << "total_effects=" << EffectCount << "\n";
    Report = Os.str();
    return true;
}

namespace
{
    // T6's FxFloatRange/FxIntRange are separate types from T5's but share the same base/amplitude
    // shape (same situation as DescribeMaterialGeneric above) - templated so ExportFxPool<TAssetFx>
    // can call these with either game's range type.
    template <typename TRange>
    void WriteFloatRange(std::ostream& Os, const char* Indent, const char* Key, const TRange& R, bool Comma = true)
    {
        Os << Indent << "\"" << Key << "\": { \"base\": " << R.base << ", \"amplitude\": " << R.amplitude << " }"
           << (Comma ? "," : "") << "\n";
    }

    template <typename TRange>
    void WriteIntRange(std::ostream& Os, const char* Indent, const char* Key, const TRange& R, bool Comma = true)
    {
        Os << Indent << "\"" << Key << "\": { \"base\": " << R.base << ", \"amplitude\": " << R.amplitude << " }"
           << (Comma ? "," : "") << "\n";
    }

    // Resolves what an element's visual union actually points to, by element type - same rule
    // OatDescribeFxInProcess uses (visualCount<=1: instance IS the union directly; >1: array[0]).
    template <typename TFxElemDef>
    auto FirstVisual(const TFxElemDef& El) -> decltype(&El.visuals.instance)
    {
        if (El.elemType > 0x6 && El.elemType != 0xA)
            return nullptr; // model(0x7)/lights(0x8,0x9)/decal(0xB)/runner(0xC) handled separately below
        return (El.visualCount <= 1) ? &El.visuals.instance
                                      : (El.visuals.array ? &El.visuals.array[0] : nullptr);
    }

    // effectOnImpact/effectOnDeath/effectEmitted/effectAttached are a union: some builds store the
    // referenced effect's *name* directly (cheap, no relocation needed), others a resolved pointer.
    // Try name first (matches how mod-facing GDT/GSC always refers to effects by name); fall back to
    // the handle's own name if that's what's actually populated.
    template <typename TFxEffectDefRef>
    std::string FxRefName(const TFxEffectDefRef& Ref)
    {
        std::string N = SafeCString(Ref.name);
        if (!N.empty())
            return N;
        if (Ref.handle && (const void*)Ref.handle > (const void*)0x10000)
            return SafeCString(Ref.handle->name);
        return "";
    }
}

namespace
{
    // Same "T6 matches T5's field names/union shapes" situation as DescribeFxPool above - one
    // template body. Real divergence: T5's FxElemDef has a `trailDef` pointer; T6's equivalent slot
    // is `extended` (an unexplored FxElemExtendedDefPtr type) - `if constexpr (requires { El.trailDef; })`
    // picks the right behaviour per game. T6 output always has trailDef: null - a known gap, not
    // silently wrong data.
    template <typename TAssetFx>
    void ExportFxPool(Zone& Z, const std::string& NeedleLower, const std::string& OutDir,
                       std::ostream& Summary, size_t& Seen, size_t& Written)
    {
        namespace fs = std::filesystem;
        std::error_code Ec;

        for (auto* Info : Z.m_pools.template PoolAssets<TAssetFx>())
        {
            const auto* Fx = Info ? Info->Asset() : nullptr;
            if (!Fx || !Fx->name)
                continue;
            Seen++;

        const std::string Name = Fx->name;
        if (!NeedleLower.empty())
        {
            std::string NameLower = Name;
            for (auto& Ch : NameLower) Ch = (char)std::tolower((unsigned char)Ch);
            if (NameLower.find(NeedleLower) == std::string::npos)
                continue;
        }

        fs::path OutPath = fs::path(OutDir) / (Name + ".json");
        fs::create_directories(OutPath.parent_path(), Ec);
        std::ofstream Out(OutPath, std::ios::out | std::ios::binary);
        if (!Out.is_open())
        {
            Summary << "FAILED to open for write: " << Name << "\n";
            continue;
        }

        const int Total = Fx->elemDefCountLooping + Fx->elemDefCountOneShot + Fx->elemDefCountEmission;

        Out << "{\n"
            << "  \"name\": \"" << JsonEscape(Name) << "\",\n"
            << "  \"flags\": " << (int)Fx->flags << ",\n"
            << "  \"priority\": " << (int)Fx->efPriority << ",\n"
            << "  \"msecLoopingLife\": " << Fx->msecLoopingLife << ",\n"
            << "  \"elemDefCountLooping\": " << Fx->elemDefCountLooping << ",\n"
            << "  \"elemDefCountOneShot\": " << Fx->elemDefCountOneShot << ",\n"
            << "  \"elemDefCountEmission\": " << Fx->elemDefCountEmission << ",\n"
            << "  \"elements\": [\n";

        if (Fx->elemDefs)
        {
            for (int i = 0; i < Total; i++)
            {
                const auto& El = Fx->elemDefs[i];
                const unsigned char ElemType = (unsigned char)El.elemType;
                const bool IsLooping = i < Fx->elemDefCountLooping;

                Out << "    {\n"
                    << "      \"index\": " << i << ",\n"
                    << "      \"type\": \"" << FxElemTypeName(ElemType) << "\",\n"
                    << "      \"flags\": " << El.flags << ",\n";

                // Spawn: looping (fixed interval/count) or one-shot (random count range) - a union,
                // discriminated by which count bucket this element index falls into.
                if (IsLooping)
                {
                    Out << "      \"spawn\": { \"kind\": \"looping\", \"intervalMsec\": "
                        << El.spawn.looping.intervalMsec << ", \"count\": " << El.spawn.looping.count << " },\n";
                }
                else
                {
                    Out << "      \"spawn\": { \"kind\": \"oneShot\", \"count\": { \"base\": "
                        << El.spawn.oneShot.count.base << ", \"amplitude\": " << El.spawn.oneShot.count.amplitude
                        << " } },\n";
                }

                WriteFloatRange(Out, "      ", "spawnRange", El.spawnRange);
                WriteFloatRange(Out, "      ", "fadeInRange", El.fadeInRange);
                WriteFloatRange(Out, "      ", "fadeOutRange", El.fadeOutRange);
                Out << "      \"spawnFrustumCullRadius\": " << El.spawnFrustumCullRadius << ",\n";
                WriteIntRange(Out, "      ", "spawnDelayMsec", El.spawnDelayMsec);
                WriteIntRange(Out, "      ", "lifeSpanMsec", El.lifeSpanMsec);
                // {x,y,z}, each a range - an object (named keys), not an array.
                Out << "      \"spawnOrigin\": {\n";
                for (int a = 0; a < 3; a++) WriteFloatRange(Out, "        ", a == 0 ? "x" : a == 1 ? "y" : "z", El.spawnOrigin[a], a < 2);
                Out << "      },\n";
                WriteFloatRange(Out, "      ", "spawnOffsetRadius", El.spawnOffsetRadius);
                WriteFloatRange(Out, "      ", "spawnOffsetHeight", El.spawnOffsetHeight);
                Out << "      \"spawnAngles\": {\n";
                for (int a = 0; a < 3; a++) WriteFloatRange(Out, "        ", a == 0 ? "x" : a == 1 ? "y" : "z", El.spawnAngles[a], a < 2);
                Out << "      },\n";
                Out << "      \"angularVelocity\": {\n";
                for (int a = 0; a < 3; a++) WriteFloatRange(Out, "        ", a == 0 ? "x" : a == 1 ? "y" : "z", El.angularVelocity[a], a < 2);
                Out << "      },\n";
                WriteFloatRange(Out, "      ", "initialRotation", El.initialRotation);
                Out << "      \"rotationAxis\": " << El.rotationAxis << ",\n";
                WriteFloatRange(Out, "      ", "gravity", El.gravity);
                WriteFloatRange(Out, "      ", "reflectionFactor", El.reflectionFactor);
                Out << "      \"atlas\": { \"behavior\": " << (int)El.atlas.behavior << ", \"index\": " << (int)El.atlas.index
                    << ", \"fps\": " << (int)El.atlas.fps << ", \"loopCount\": " << (int)El.atlas.loopCount
                    << ", \"colIndexBits\": " << (int)El.atlas.colIndexBits << ", \"rowIndexBits\": " << (int)El.atlas.rowIndexBits
                    << " },\n";
                Out << "      \"windInfluence\": " << El.windInfluence << ",\n";
                Out << "      \"sortOrder\": " << (int)El.sortOrder << ",\n";
                Out << "      \"lightingFrac\": " << (int)El.lightingFrac << ",\n";
                Out << "      \"alphaFadeTimeMsec\": " << El.alphaFadeTimeMsec << ",\n";
                // T5's billboardPivot is a raw float[2]; T6's is `union vec2_t { float v[2]; struct
                // { float x, y; }; }` - no operator[] of its own, but its .v member is indexable.
                // if constexpr picks the right access per game at compile time.
                if constexpr (requires { El.billboardPivot[0]; })
                    Out << "      \"billboardPivot\": [" << El.billboardPivot[0] << ", " << El.billboardPivot[1] << "],\n";
                else
                    Out << "      \"billboardPivot\": [" << El.billboardPivot.v[0] << ", " << El.billboardPivot.v[1] << "],\n";

                // Resolved visual reference, by element type: what this element actually LOOKS like
                // (material/model/sound), not just its blend mode. Every pointer here is a raw field
                // out of a union whose active member depends on elemType, so SafeCString bounds-checks
                // the pointer before touching what it points at.
                auto AsMaterialPtr = [](auto* P) { // returns the same Material* type as passed, or nullptr
                    return ((const void*)P > (const void*)0x10000) ? P : decltype(P)(nullptr);
                };
                Out << "      \"visual\": ";
                if (ElemType <= 0x6)
                {
                    const auto* V = FirstVisual(El);
                    const auto* Mat = (V && (const void*)V->material > (const void*)0x10000) ? V->material : nullptr;
                    const std::string Mn = Mat ? SafeCString(Mat->info.name) : std::string();
                    Out << "{ \"kind\": \"material\", \"name\": " << (Mn.empty() ? "null" : ("\"" + JsonEscape(Mn) + "\"")) << " }";
                }
                else if (ElemType == 0x7)
                {
                    const void* Mp = El.visuals.instance.model;
                    const auto* Model = ((const void*)Mp > (const void*)0x10000) ? El.visuals.instance.model : nullptr;
                    const std::string Mn = Model ? SafeCString(Model->name) : std::string();
                    Out << "{ \"kind\": \"model\", \"name\": " << (Mn.empty() ? "null" : ("\"" + JsonEscape(Mn) + "\"")) << " }";
                }
                else if (ElemType == 0xA)
                {
                    const std::string Sn = SafeCString(El.visuals.instance.soundName);
                    Out << "{ \"kind\": \"sound\", \"alias\": " << (Sn.empty() ? "null" : ("\"" + JsonEscape(Sn) + "\"")) << " }";
                }
                else if (ElemType == 0xB && (const void*)El.visuals.markArray > (const void*)0x10000)
                {
                    const auto* M0 = AsMaterialPtr(El.visuals.markArray->materials[0]);
                    const auto* M1 = AsMaterialPtr(El.visuals.markArray->materials[1]);
                    const std::string N0 = M0 ? SafeCString(M0->info.name) : std::string();
                    const std::string N1 = M1 ? SafeCString(M1->info.name) : std::string();
                    Out << "{ \"kind\": \"decal\", \"material\": " << (N0.empty() ? "null" : ("\"" + JsonEscape(N0) + "\""))
                        << ", \"material2\": " << (N1.empty() ? "null" : ("\"" + JsonEscape(N1) + "\"")) << " }";
                }
                else
                {
                    Out << "null";
                }
                Out << ",\n";

                // Effect chaining - what this element spawns/triggers next, by name where resolvable.
                const std::string OnImpact = FxRefName(El.effectOnImpact);
                const std::string OnDeath = FxRefName(El.effectOnDeath);
                const std::string Emitted = FxRefName(El.effectEmitted);
                const std::string Attached = FxRefName(El.effectAttached);
                Out << "      \"effectOnImpact\": " << (OnImpact.empty() ? "null" : ("\"" + JsonEscape(OnImpact) + "\"")) << ",\n";
                Out << "      \"effectOnDeath\": " << (OnDeath.empty() ? "null" : ("\"" + JsonEscape(OnDeath) + "\"")) << ",\n";
                Out << "      \"effectEmitted\": " << (Emitted.empty() ? "null" : ("\"" + JsonEscape(Emitted) + "\"")) << ",\n";
                WriteFloatRange(Out, "      ", "emitDist", El.emitDist);
                WriteFloatRange(Out, "      ", "emitDistVariance", El.emitDistVariance);
                Out << "      \"effectAttached\": " << (Attached.empty() ? "null" : ("\"" + JsonEscape(Attached) + "\"")) << ",\n";

                // Trail geometry, if this is a TRAIL element (0x5) with a trailDef - T5 only; T6's
                // equivalent slot is `extended` (unexplored) so this compiles out for T6.
                if constexpr (requires { El.trailDef; })
                {
                    if (ElemType == 0x5 && El.trailDef)
                    {
                        Out << "      \"trailDef\": { \"scrollTimeMsec\": " << El.trailDef->scrollTimeMsec
                            << ", \"repeatDist\": " << El.trailDef->repeatDist << ", \"splitDist\": " << El.trailDef->splitDist
                            << ", \"vertCount\": " << El.trailDef->vertCount << ", \"indCount\": " << El.trailDef->indCount << " }\n";
                    }
                    else
                    {
                        Out << "      \"trailDef\": null\n";
                    }
                }
                else
                {
                    Out << "      \"trailDef\": null\n";
                }

                Out << "    }" << (i + 1 < Total ? "," : "") << "\n";
            }
        }

        Out << "  ]\n}\n";
        Out.close();
        Written++;
        Summary << "wrote " << Name << ".json (" << Total << " elements)\n";
        }
    }
}

bool OatExportFxInProcess(const std::string& FastFilePath, const std::string& NameFilter,
                           const std::string& OutDir, std::string& Report)
{
    std::unique_ptr<Zone> Z = LoadZoneOrReport(FastFilePath, Report);
    if (!Z)
        return false;
    if (Z->m_game_id != GameId::T5 && Z->m_game_id != GameId::T6)
    {
        Report = "fx export: only T5 (BO1) and T6 (BO2) supported for now";
        return false;
    }

    std::string NeedleLower = NameFilter;
    for (auto& Ch : NeedleLower) Ch = (char)std::tolower((unsigned char)Ch);

    std::error_code Ec;
    std::filesystem::create_directories(OutDir, Ec);

    std::ostringstream Summary;
    size_t Written = 0, Seen = 0;

    if (Z->m_game_id == GameId::T5)
        ExportFxPool<T5::AssetFx>(*Z, NeedleLower, OutDir, Summary, Seen, Written);
    else
        ExportFxPool<T6::AssetFx>(*Z, NeedleLower, OutDir, Summary, Seen, Written);

    Summary << "total_effects_in_zone=" << Seen << " written=" << Written << "\n";
    Report = Summary.str();
    return true;
}

namespace
{
    // T6::XModel matches T5::XModel's field names/types for everything read here (name, numBones,
    // numsurfs, materialHandles, numCollSurfs, contents, radius) - unverified against a real T6 zone.
    template <typename TAssetXModel>
    void ListXModelMaterialsPool(Zone& Z, std::ostream& Os, size_t& ModelCount)
    {
        for (auto* Info : Z.m_pools.template PoolAssets<TAssetXModel>())
        {
            const auto* Model = Info ? Info->Asset() : nullptr;
            if (!Model || !Model->materialHandles)
                continue;
            ModelCount++;

            Os << "xmodel: " << (Model->name ? Model->name : "?") << " (" << (int)Model->numsurfs << " surfaces)"
               << " radius=" << Model->radius << " numBones=" << (int)Model->numBones
               << " numCollSurfs=" << Model->numCollSurfs << " contents=" << Model->contents << "\n";
            for (unsigned char s = 0; s < Model->numsurfs; s++)
            {
                Os << "  surf " << (int)s << ":" << DescribeMaterialGeneric(Model->materialHandles[s]) << "\n";
            }
        }
    }
}

bool OatListXModelMaterialsInProcess(const std::string& FastFilePath, std::string& Report)
{
    std::unique_ptr<Zone> Z = LoadZoneOrReport(FastFilePath, Report);
    if (!Z)
        return false;
    if (Z->m_game_id != GameId::T5 && Z->m_game_id != GameId::T6)
    {
        Report = "xmodel materials: only T5 (BO1) and T6 (BO2) supported for now";
        return false;
    }

    std::ostringstream Os;
    size_t ModelCount = 0;

    if (Z->m_game_id == GameId::T5)
        ListXModelMaterialsPool<T5::AssetXModel>(*Z, Os, ModelCount);
    else
        ListXModelMaterialsPool<T6::AssetXModel>(*Z, Os, ModelCount);

    Os << "total_xmodels=" << ModelCount << "\n";
    Report = Os.str();
    return true;
}

namespace
{
    // Shared across T5/T6 - field names match in both (checked against T6_Assets.h). Takes just
    // the WeaponDef pointer, not the whole variant, so the T5/T6 dispatchers below can format the
    // parts that DO differ (damage model) themselves before/after calling this.
    template <typename TWeaponDef>
    void DescribeWeaponCommon(std::ostream& Os, const TWeaponDef* D)
    {
        Os << "  explosionRadius=" << D->iExplosionRadius << " explosionRadiusMin=" << D->iExplosionRadiusMin << "\n"
           << "  explosionInnerDamage=" << D->iExplosionInnerDamage << " explosionOuterDamage=" << D->iExplosionOuterDamage << "\n"
           << "  destabilizeDistance=" << D->destabilizeDistance
           << " destabilizationRateTime=" << D->destabilizationRateTime
           << " destabilizationCurvatureMax=" << D->destabilizationCurvatureMax << "\n"
           << "  damageConeAngle=" << D->damageConeAngle << " maxDist=" << D->maxDist << " fightDist=" << D->fightDist << "\n";
    }
}

bool OatDescribeWeaponInProcess(const std::string& FastFilePath, const std::string& NameFilter, std::string& Report)
{
    std::unique_ptr<Zone> Z = LoadZoneOrReport(FastFilePath, Report);
    if (!Z)
        return false;
    if (Z->m_game_id != GameId::T5 && Z->m_game_id != GameId::T6)
    {
        Report = "weapon: only T5 (BO1) and T6 (BO2) supported for now";
        return false;
    }

    std::string NeedleLower = NameFilter;
    for (auto& Ch : NeedleLower) Ch = (char)std::tolower((unsigned char)Ch);

    std::ostringstream Os;
    size_t WeaponCount = 0;

    if (Z->m_game_id == GameId::T5)
    {
        for (auto* Info : Z->m_pools.PoolAssets<T5::AssetWeapon>())
        {
            const T5::WeaponVariantDef* Variant = Info ? Info->Asset() : nullptr;
            if (!Variant || !Variant->weapDef) continue;
            const std::string Name = Info->m_name;
            if (!NeedleLower.empty())
            {
                std::string NameLower = Name;
                for (auto& Ch : NameLower) Ch = (char)std::tolower((unsigned char)Ch);
                if (NameLower.find(NeedleLower) == std::string::npos) continue;
            }
            WeaponCount++;
            const T5::WeaponDef* D = Variant->weapDef;
            Os << "weapon: " << Name << "\n";
            // T5-only fields: a flat min/max damage-by-distance pair. T6 replaced this with a
            // damage[6]/damageRange[6] tiered array (see the T6 branch below) - genuinely
            // different data, not a rename, so this stays a separate block rather than forcing
            // both into DescribeWeaponCommon().
            Os << "  minDamage=" << D->minDamage << " minPlayerDamage=" << D->minPlayerDamage << "\n"
               << "  minDamageRange=" << D->fMinDamageRange << " maxDamageRange=" << D->fMaxDamageRange << "\n";
            DescribeWeaponCommon(Os, D);
        }
    }
    else   // T6: field presence/names checked against T6_Assets.h only.
    {
        for (auto* Info : Z->m_pools.PoolAssets<T6::AssetWeapon>())
        {
            const T6::WeaponVariantDef* Variant = Info ? Info->Asset() : nullptr;
            if (!Variant || !Variant->weapDef) continue;
            const std::string Name = Info->m_name;
            if (!NeedleLower.empty())
            {
                std::string NameLower = Name;
                for (auto& Ch : NameLower) Ch = (char)std::tolower((unsigned char)Ch);
                if (NameLower.find(NeedleLower) == std::string::npos) continue;
            }
            WeaponCount++;
            const T6::WeaponDef* D = Variant->weapDef;
            Os << "weapon: " << Name << "\n";
            // T6's damage model: damage[6]/damageRange[6] tiered arrays (index meaning - e.g.
            // per-difficulty or per-falloff-tier - not confirmed; printed positionally).
            Os << "  damage=[";
            for (int i = 0; i < 6; i++) Os << D->damage[i] << (i < 5 ? "," : "");
            Os << "] damageRange=[";
            for (int i = 0; i < 6; i++) Os << D->damageRange[i] << (i < 5 ? "," : "");
            Os << "] minPlayerDamage=" << D->minPlayerDamage << "\n";
            DescribeWeaponCommon(Os, D);
        }
    }

    Os << "total_weapons=" << WeaponCount << "\n";
    Report = Os.str();
    return true;
}

namespace
{
    // T5 vs T6 rawfile compression: which extension is compressed, and the header/deflate format,
    // both differ - and T6::RawFile::buffer's `char16` type name is misleading (still 1-byte char).
    template <typename TAssetRawFile>
    void ExportRawfilePool(Zone& Z, const std::string& NeedleLower, const std::string& OutDir,
                            std::ostream& Os, size_t& Seen, size_t& Written, bool IsT6)
    {
        namespace fs = std::filesystem;
        std::error_code Ec;

        for (auto* Info : Z.m_pools.template PoolAssets<TAssetRawFile>())
        {
            const auto* Raw = Info ? Info->Asset() : nullptr;
            if (!Raw || !Raw->name || !Raw->buffer || Raw->len <= 0)
                continue;
            Seen++;

            const std::string Name = Raw->name;
            if (!NeedleLower.empty())
            {
                std::string NameLower = Name;
                for (auto& Ch : NameLower) Ch = (char)std::tolower((unsigned char)Ch);
                if (NameLower.find(NeedleLower) == std::string::npos)
                    continue;
            }

            const char* RawBuf = reinterpret_cast<const char*>(Raw->buffer); // both games: 1-byte
            std::vector<char> Content;
            bool Transformed = false;

            const bool NeedsDecompress = IsT6
                ? (Name.size() > 4 && Name.compare(Name.size() - 4, 4, ".atr") == 0)
                : (Name.size() > 4 && (Name.compare(Name.size() - 4, 4, ".gsc") == 0 || Name.compare(Name.size() - 4, 4, ".csc") == 0));

            if (NeedsDecompress && IsT6 && Raw->len > 4)
            {
                const auto* P = reinterpret_cast<const unsigned char*>(RawBuf);
                const uint32_t DecompSize = (uint32_t)P[0] | ((uint32_t)P[1] << 8) | ((uint32_t)P[2] << 16) | ((uint32_t)P[3] << 24);
                if (DecompSize > 0 && DecompSize < 64 * 1024 * 1024)
                {
                    std::vector<char> Out(DecompSize);
                    z_stream Zs{};
                    if (inflateInit2(&Zs, -15) == Z_OK) // raw deflate, no zlib wrapper
                    {
                        Zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(RawBuf + 4));
                        Zs.avail_in = (uInt)((size_t)Raw->len - 4);
                        Zs.next_out = reinterpret_cast<Bytef*>(Out.data());
                        Zs.avail_out = (uInt)Out.size();
                        const int Zr = inflate(&Zs, Z_FINISH);
                        const size_t Produced = Out.size() - Zs.avail_out;
                        inflateEnd(&Zs);
                        if (Zr == Z_STREAM_END || (Zr == Z_OK && Produced > 0))
                        {
                            Out.resize(Produced);
                            Content = std::move(Out);
                            Transformed = true;
                        }
                        else
                        {
                            Os << "raw-deflate inflate FAILED (code " << Zr << ") for " << Name << ", writing raw bytes instead\n";
                        }
                    }
                }
            }
            else if (NeedsDecompress && !IsT6 && Raw->len > 8)
            {
                const auto* P = reinterpret_cast<const unsigned char*>(RawBuf);
                const uint32_t DecompSize = (uint32_t)P[0] | ((uint32_t)P[1] << 8) | ((uint32_t)P[2] << 16) | ((uint32_t)P[3] << 24);
                const uint32_t CompSize   = (uint32_t)P[4] | ((uint32_t)P[5] << 8) | ((uint32_t)P[6] << 16) | ((uint32_t)P[7] << 24);
                if (DecompSize > 0 && CompSize > 0 && (uint64_t)8 + CompSize <= (uint64_t)Raw->len)
                {
                    Content.resize(DecompSize);
                    uLongf DestLen = DecompSize;
                    const int Zr = uncompress(reinterpret_cast<Bytef*>(Content.data()), &DestLen,
                                               reinterpret_cast<const Bytef*>(RawBuf + 8), CompSize);
                    if (Zr == Z_OK) { Content.resize(DestLen); Transformed = true; }
                    else { Os << "zlib decompress FAILED (code " << Zr << ") for " << Name << ", writing raw bytes instead\n"; Content.clear(); }
                }
            }

            if (!Transformed)
                Content.assign(RawBuf, RawBuf + Raw->len);

            fs::path OutPath = fs::path(OutDir) / Name;
            fs::create_directories(OutPath.parent_path(), Ec);
            std::ofstream Out(OutPath, std::ios::out | std::ios::binary);
            if (!Out.is_open())
            {
                Os << "FAILED to open for write: " << Name << "\n";
                continue;
            }
            Out.write(Content.data(), (std::streamsize)Content.size());
            Out.close();
            Written++;
            Os << "wrote " << Name << " (" << Content.size() << " bytes" << (Transformed ? ", decoded" : "") << ") -> " << OutPath.string() << "\n";
        }
    }
}

bool OatDumpRawfilesInProcess(const std::string& FastFilePath, const std::string& NameFilter,
                               const std::string& OutDir, std::string& Report)
{
    std::unique_ptr<Zone> Z = LoadZoneOrReport(FastFilePath, Report);
    if (!Z)
        return false;
    if (Z->m_game_id != GameId::T5 && Z->m_game_id != GameId::T6)
    {
        Report = "rawfile: only T5 (BO1) and T6 (BO2) supported for now";
        return false;
    }

    std::string NeedleLower = NameFilter;
    for (auto& Ch : NeedleLower) Ch = (char)std::tolower((unsigned char)Ch);

    std::error_code Ec;
    std::filesystem::create_directories(OutDir, Ec);

    std::ostringstream Os;
    size_t Written = 0, Seen = 0;

    if (Z->m_game_id == GameId::T5)
        ExportRawfilePool<T5::AssetRawFile>(*Z, NeedleLower, OutDir, Os, Seen, Written, false);
    else
        ExportRawfilePool<T6::AssetRawFile>(*Z, NeedleLower, OutDir, Os, Seen, Written, true);

    Os << "total_rawfiles_in_zone=" << Seen << " written=" << Written << "\n";
    Report = Os.str();
    return true;
}

namespace
{
    // T5 stores vec3/axis placement fields as plain float arrays; T6 stores the same data as
    // vec3_t/GfxPackedPlacement.axis[3] unions (same memory layout, no operator[] of their own - same
    // situation as FxElemDef::billboardPivot above). These hand back a plain contiguous const float*
    // so GltfSceneWriter's math stays identical for both games.
    template <typename TVec3>
    const float* Vec3Floats(const TVec3& V)
    {
        if constexpr (requires { V[0]; })
            return &V[0];
        else
            return &V.v[0];
    }

    template <typename TPlacement>
    const float* AxisFloats9(const TPlacement& Pl)
    {
        if constexpr (requires { Pl.axis[0][0]; })
            return &Pl.axis[0][0]; // T5: float axis[3][3], already contiguous
        else
            return &Pl.axis[0].v[0]; // T6: vec3_t axis[3] - contiguous union storage, same 9-float layout
    }

    // ---- the map scene (MapScene.h) out of OAT's assets, for the one map writer (MapGlbWriter.h)

    bool ReadPng(const std::filesystem::path& Path, std::vector<uint8_t>& Out)
    {
        std::ifstream F(Path, std::ios::binary);
        Out.assign((std::istreambuf_iterator<char>(F)), std::istreambuf_iterator<char>());
        return !Out.empty();
    }

    // A material by name, first seen wins, its colour map the colorMap texture (0xA0AB1041)
    void AddOatMaterial(Maps::MapScene& S, const std::string& Name, const std::string& ColorMap)
    {
        Maps::Material& M = S.Materials[Name];
        if (!M.Name.empty()) return;
        M.Name = Name;
        if (ColorMap.empty()) return;
        M.ColorMap = ColorMap;
        M.Textures.push_back({ 0xA0AB1041u, ColorMap });
    }

    Maps::Vertex ToMapVertex(const GltfSceneWriter::Vertex& V)
    {
        Maps::Vertex O;
        for (int K = 0; K < 3; K++) { O.Pos[K] = V.pos[K]; O.Normal[K] = V.normal[K]; }
        O.Uv[0] = V.uv[0]; O.Uv[1] = V.uv[1];
        for (int K = 0; K < 4; K++) O.Color[K] = V.color[K];
        return O;
    }

    Maps::Surface OatSurface(const std::string& Material, const std::vector<GltfSceneWriter::Vertex>& Verts,
                             const std::vector<uint32_t>& Indices)
    {
        Maps::Surface Sf;
        Sf.Material = Material;
        Sf.Vertices.reserve(Verts.size());
        for (const auto& V : Verts) Sf.Vertices.push_back(ToMapVertex(V));
        Sf.Indices = Indices;
        return Sf;
    }

    // One XModel converted by OAT (LOD 0) as a scene model; its name ("" without geometry). OAT's
    // faces already wind the glTF way: stored reversed, for the writer reverses every triangle.
    std::string OatModel(Maps::MapScene& S, const XModelCommon* Common, const char* FallbackMatName,
                         const std::function<bool(const std::string&)>& Texture)
    {
        if (!Common) return std::string();
        Maps::Mesh M;
        M.Name = Common->m_name;
        for (const auto& Obj : Common->m_objects)
        {
            Maps::Surface Sf;
            std::unordered_map<unsigned, uint32_t> Remap;
            for (const auto& F : Obj.m_faces)
            {
                static const int Order[3] = { 0, 2, 1 };
                uint32_t Tri[3];
                bool Ok = true;
                for (int k = 0; k < 3 && Ok; k++)
                {
                    const unsigned gi = F.vertexIndex[Order[k]];
                    if (gi >= Common->m_vertices.size()) { Ok = false; break; }
                    auto [It, New] = Remap.try_emplace(gi, (uint32_t)Sf.Vertices.size());
                    if (New) Sf.Vertices.push_back(ToMapVertex(ToGltfVertex(Common->m_vertices[gi])));
                    Tri[k] = It->second;
                }
                if (Ok) Sf.Indices.insert(Sf.Indices.end(), Tri, Tri + 3);
            }
            if (Sf.Vertices.empty() || Sf.Indices.empty()) continue;
            std::string MatName = FallbackMatName, ColorMap;
            if (Obj.materialIndex >= 0 && (size_t)Obj.materialIndex < Common->m_materials.size())
            {
                const auto& CMat = Common->m_materials[Obj.materialIndex];
                if (!CMat.name.empty()) MatName = CMat.name;
                ColorMap = CMat.colorMapName;
                Texture(ColorMap);
            }
            AddOatMaterial(S, MatName, ColorMap);
            Sf.Material = MatName;
            M.Surfaces.push_back(std::move(Sf));
        }
        if (M.Surfaces.empty()) return std::string();
        std::string Name = M.Name;
        for (int N = 2; S.Models.count(Name); N++) Name = M.Name + "#" + std::to_string(N);
        S.Models[Name] = std::move(M);
        return Name;
    }

    Maps::Instance OatInstance(const std::string& Name, const std::string& Model, const float Axis[9],
                               const float Origin[3], float Scale)
    {
        Maps::Instance In;
        In.Name = Name;
        In.Model = Model;
        for (int K = 0; K < 9; K++) In.Axis[K] = Axis[K];
        for (int K = 0; K < 3; K++) In.Origin[K] = Origin[K];
        In.Scale = Scale;
        return In;
    }

    // ComWorld primary light: type 1 the sun, 2 a spot, the rest omni
    Maps::Light OatLight(unsigned char Type, const float Origin[3], const float Dir[3], const float Color[3],
                         float Radius, float CosHalfFovOuter)
    {
        Maps::Light L;
        L.Kind = Type == 1 ? Maps::Light::Type::Sun : Type == 2 ? Maps::Light::Type::Spot : Maps::Light::Type::Point;
        for (int K = 0; K < 3; K++) { L.Origin[K] = Origin[K]; L.Dir[K] = Dir[K]; L.Color[K] = Color[K]; }
        L.Radius = Radius;
        L.CosHalfFovOuter = CosHalfFovOuter;
        return L;
    }

    // T6 (BO2) map-texture extraction: a scoped image-only dump of an already-loaded zone, reusing the
    // same two halves OatDumpZoneInProcess uses for a full dump (OAT's writer for embedded images,
    // ExtractT6ZoneImages for IPAK-streamed ones) but restricted to AssetImage so a map's ~1800 images
    // don't drag every model/sound along. Loads its own Zone from a fastfile PATH so it can be called
    // once per relevant zone (the map's own .ff plus any "common" zone owning textures the map's own
    // pool doesn't have), all writing into the same OutDir. Silently returns 0 on a load failure - a
    // missing/renamed common zone shouldn't hard-fail a map export.
    size_t DumpT6ZoneImages(const std::string& FastFilePath, const std::string& OutDir)
    {
        std::string LoadReport;
        std::unique_ptr<Zone> ZonePtr = LoadZoneOrReport(FastFilePath, LoadReport);
        if (!ZonePtr)
            return 0;
        Zone& Z = *ZonePtr;

        const GameId Game = Z.m_game_id;
        const IGame* GameInfo = IGame::GetGameById(Game);
        if (!GameInfo)
            return 0;

        std::error_code Ec;
        const std::string ZoneDir = std::filesystem::absolute(
            std::filesystem::path(FastFilePath).remove_filename(), Ec).string();

        SharedSearchPaths Paths;
        const auto SearchPathsForZone = AutoSearchPaths::GetForGame(Game)->GetSearchPathsForZonePath(ZoneDir);
        for (const auto& Sp : SearchPathsForZone)
            Paths.RefSearchPath(Sp);

        const IObjLoader* ObjLoader = IObjLoader::GetObjLoaderForGame(Game);
        ObjLoader->LoadReferencedContainersForZone(Paths.GetSearchPath(), Z);

        std::vector<bool> Bitfield((size_t)GameInfo->GetAssetTypeCount(), false);
        if ((size_t)T6::ASSET_TYPE_IMAGE < Bitfield.size())
            Bitfield[T6::ASSET_TYPE_IMAGE] = true;
        ObjWriting::Configuration.AssetTypesToHandleBitfield = Bitfield;
        ObjWriting::Configuration.ImageOutputFormat = ImageOutputFormat_e::DDS;

        std::filesystem::create_directories(OutDir, Ec);
        OutputPathFilesystem OutputPath(OutDir);
        AssetDumpingContext Context(Z, OutDir, OutputPath, Paths.GetSearchPath(), std::nullopt);

        IObjWriter* ObjWriter = IObjWriter::GetObjWriterForGame(Game);
        ObjWriter->DumpZone(Context);

        ObjLoader->UnloadContainersOfZone(Z);

        return ExtractT6ZoneImages(Z, OutDir, ZoneDir); // the IPAK-streamed half, written as PNG directly
    }

    // Converts a .dds file on disk (as OAT's image writer writes for embedded images) to PNG
    // through Image::ConvertImageMemory; DDS_WithHeader reads the format from the file's header.
    bool ConvertDdsFileToPng(const std::string& DdsPath, const std::string& OutPngPath)
    {
        std::ifstream F(DdsPath, std::ios::binary);
        if (!F)
            return false;
        std::vector<int8_t> Bytes((std::istreambuf_iterator<char>(F)), std::istreambuf_iterator<char>());
        if (Bytes.empty())
            return false;
        return Image::ConvertImageMemory(Bytes.data(), Bytes.size(), ImageFormat::DDS_WithHeader,
                                         OutPngPath, ImageFormat::Standard_PNG, ImagePatch::NoPatch);
    }

    // T6 (BO2) map export: world mesh + props + lights. T5's plain GfxWorldVertex* array is replaced
    // in T6 by an opaque packed byte blob (GfxWorldDraw::vd0) with no OAT layout; its decoding
    // and the vertex stride are below (the T6 packed-vertex format). Skybox: T6's is a
    // real XModel via skyBoxModel, not a flat texture like T5's skyImage - exported as geometry.
    bool ExportMapGlbT6(Zone& Z, const std::string& FastFilePath, const std::string& OutGlbPath,
                        std::string& Report, bool EmbedTextures)
    {
        const T6::GfxWorld* Gw = nullptr;
        for (auto* Info : Z.m_pools.PoolAssets<T6::AssetGfxWorld>())
            if (Info && Info->Asset()) { Gw = Info->Asset(); break; }
        if (!Gw) { Report = "no GfxWorld in zone (not a map fastfile?)"; return false; }

        // --- world mesh: decode per the layout derived above ---
        const uint8_t* const VertBlock = (Gw->draw.vertexCount && Gw->draw.vd0.data)
            ? reinterpret_cast<const uint8_t*>(Gw->draw.vd0.data) : nullptr;
        // vertexDataSize0/vertexCount doesn't divide evenly on a real map, but the floor-divided value
        // is the per-vertex stride. Used only
        // if it's a plausible vertex size; every read is still bounds-checked against vertexDataSize0.
        const uint32_t StrideGuess = Gw->draw.vertexCount ? (Gw->draw.vertexDataSize0 / Gw->draw.vertexCount) : 0u;
        const uint32_t VertexStride = (VertBlock && StrideGuess >= 32 && StrideGuess <= 128) ? StrideGuess : 0u;
        const uint16_t* const IB = Gw->draw.indices;
        const int IC = Gw->draw.indexCount;
        const bool WorldMeshOk = VertBlock && VertexStride != 0 && IB && Gw->dpvs.surfaces && Gw->surfaceCount > 0;
        std::string WorldMeshNote;
        if (!WorldMeshOk)
        {
            WorldMeshNote = !VertBlock ? "no vertex data"
                          : VertexStride == 0 ? "vertexDataSize0/vertexCount didn't produce a plausible stride"
                          : "no indices/surfaces";
        }

        auto DecodeVertex = [&](uint32_t ByteAddr, GltfSceneWriter::Vertex& Out) -> bool {
            if (ByteAddr + VertexStride > Gw->draw.vertexDataSize0)
                return false;
            const uint8_t* P = VertBlock + ByteAddr;
            float Xyz[3]; std::memcpy(Xyz, P, sizeof(Xyz));
            Out.pos[0] = Xyz[0]; Out.pos[1] = Xyz[1]; Out.pos[2] = Xyz[2];
            Out.color[0] = P[16]; Out.color[1] = P[17]; Out.color[2] = P[18]; Out.color[3] = P[19];
            T6::PackedTexCoords Tc; std::memcpy(&Tc.packed, P + 20, sizeof(Tc.packed));
            float Uv[2] = {0, 0};
            T6::Common::Vec2UnpackTexCoords(Tc, Uv);
            Out.uv[0] = Uv[0]; Out.uv[1] = Uv[1];
            T6::PackedUnitVec Nrm; std::memcpy(&Nrm.packed, P + 28, sizeof(Nrm.packed));
            float N[3] = {0, 0, 1};
            T6::Common::Vec3UnpackUnitVec(Nrm, N);
            // Defensive: unaddressed vertex-block slots decode to non-physical values (magnitude
            // ~0.003-0.016, nowhere near a unit vector) - fall back to a plausible default instead of
            // feeding garbage lighting into the mesh.
            const float Mag2 = N[0] * N[0] + N[1] * N[1] + N[2] * N[2];
            if (Mag2 > 0.5f && Mag2 < 2.0f) { Out.normal[0] = N[0]; Out.normal[1] = N[1]; Out.normal[2] = N[2]; }
            else { Out.normal[0] = 0; Out.normal[1] = 0; Out.normal[2] = 1; }
            return true;
        };

        struct Accum {
            std::vector<GltfSceneWriter::Vertex> verts;
            std::vector<uint32_t> idx;
            std::unordered_map<uint32_t, uint32_t> remap; // key: byte address into vd0 (real maps fit in 32 bits)
        };
        FirstSeenMap<const T6::Material*, Accum> WorldGroups; // one glTF mesh per material, same as T5

        size_t WorldTris = 0;
        if (WorldMeshOk)
        {
            for (unsigned s = 0; s < Gw->surfaceCount; s++)
            {
                const T6::GfxSurface& Sf = Gw->dpvs.surfaces[s];
                Accum& G = WorldGroups[Sf.material];
                const int Base = Sf.tris.baseIndex;
                const uint32_t Vdo = (uint32_t)Sf.tris.vertexDataOffset0;

                for (unsigned t = 0; t < Sf.tris.triCount; t++)
                {
                    const int i0 = Base + (int)t * 3;
                    if (i0 < 0 || i0 + 2 >= IC) break;
                    static const int Order[3] = {0, 1, 2}; // the game's winding: the map writer reverses it
                    uint32_t Tri[3];
                    bool Ok = true;
                    for (int k = 0; k < 3; k++)
                    {
                        const uint16_t LocalIdx = IB[i0 + Order[k]];
                        const uint32_t ByteAddr = Vdo + (uint32_t)LocalIdx * VertexStride;
                        auto It = G.remap.find(ByteAddr);
                        if (It != G.remap.end()) { Tri[k] = It->second; continue; }
                        GltfSceneWriter::Vertex Ov;
                        if (!DecodeVertex(ByteAddr, Ov)) { Ok = false; break; }
                        const uint32_t Local = (uint32_t)G.verts.size();
                        G.verts.push_back(Ov);
                        G.remap.emplace(ByteAddr, Local);
                        Tri[k] = Local;
                    }
                    if (Ok) { G.idx.push_back(Tri[0]); G.idx.push_back(Tri[1]); G.idx.push_back(Tri[2]); WorldTris++; }
                }
            }
        }

        // Textures: BO2 has no main IWD archives, so ExtractIwiToPng (T5's path) can't work here - GfxImages
        // stream from IPAK/zone-referenced containers or sit embedded in the fastfile instead.
        // DumpT6ZoneImages does one scoped image-only dump of THIS zone covering both cases up front,
        // so every material's colorMap just needs a file-name lookup (+ DDS->PNG for the embedded half).
        std::error_code Ec;
        Maps::MapScene Scene;
        Scene.Name = Gw->name ? Gw->name : "";
        Scene.Game = "bo2";

        std::filesystem::create_directories(std::filesystem::path(OutGlbPath).parent_path(), Ec);

        const std::filesystem::path TexTmp =
            std::filesystem::temp_directory_path() / ("gxmaptex6_" + std::filesystem::path(OutGlbPath).stem().string());
        const std::filesystem::path TexImgDir = TexTmp / "images";
        size_t TexturesFound = 0;
        std::vector<std::string> CommonZonesTried;
        if (EmbedTextures)
        {
            std::filesystem::create_directories(TexTmp, Ec);
            DumpT6ZoneImages(FastFilePath, TexTmp.string());

            // A map's own AssetImage pool only holds images it owns; shared assets (killstreak vehicles...)
            // live in "common" zones, while a material's GfxImage* still resolves by name. So the common
            // zones are dumped into the same folder too: common.ff/common_mp.ff/common_patch_mp.ff for every
            // map (the asset path can't tell the game mode), common_zm.ff only for "zm_" maps. Missing ones
            // are skipped.
            const std::string MapName = Gw->name ? Gw->name : "";
            const std::filesystem::path ZoneDirPath = std::filesystem::path(FastFilePath).parent_path();
            std::vector<std::string> Candidates = {"common.ff", "common_mp.ff", "common_patch_mp.ff"};
            if (MapName.find("zm_") != std::string::npos)
                Candidates.push_back("common_zm.ff");
            for (const auto& C : Candidates)
            {
                const std::filesystem::path CommonPath = ZoneDirPath / C;
                std::error_code CEc;
                if (std::filesystem::exists(CommonPath, CEc))
                {
                    const size_t N = DumpT6ZoneImages(CommonPath.string(), TexTmp.string());
                    CommonZonesTried.push_back(C + " (+" + std::to_string(N) + " streamed)");
                }
            }
        }

        // Some GfxImage pool entries are assets in name only: streamedPartCount==0 and pixels==nullptr
        // means no pixel data anywhere (`,shadow`: a GPU-generated render target). T6's unmarked
        // equivalent of T5's `$`-prefixed engine defaults, recognized by asking the pool entry.
        std::unordered_map<std::string, const T6::GfxImage*> ImageByName;
        for (auto* Info : Z.m_pools.PoolAssets<T6::AssetImage>())
        {
            const T6::GfxImage* Img = Info ? Info->Asset() : nullptr;
            if (Img && Img->name)
                ImageByName.emplace(Img->name, Img);
        }
        auto IsRealTexture = [&](const std::string& Name) -> bool {
            auto It = ImageByName.find(Name);
            if (It == ImageByName.end())
                return true; // not in THIS zone's own pool - still worth trying (may be a common-zone asset)
            const T6::GfxImage* Img = It->second;
            return Img->streamedPartCount > 0 || Img->pixels != nullptr;
        };

        // A texture's PNG bytes into the scene (once per name); false when it has none.
        std::unordered_map<std::string, bool> ImgCache;
        auto ResolveTextureByName = [&](const std::string& ImgName) -> bool {
            if (!EmbedTextures || ImgName.empty() || ImgName[0] == '$' || !IsRealTexture(ImgName))
                return false;
            auto It = ImgCache.find(ImgName);
            if (It != ImgCache.end())
                return It->second;
            const std::filesystem::path Png = TexImgDir / (ImgName + ".png");
            const std::filesystem::path Dds = TexImgDir / (ImgName + ".dds");
            std::error_code FEc;
            bool HavePng = std::filesystem::exists(Png, FEc);
            if (!HavePng && std::filesystem::exists(Dds, FEc))
                HavePng = ConvertDdsFileToPng(Dds.string(), Png.string());
            const bool Found = HavePng && ReadPng(Png, Scene.Images[ImgName].Png);
            if (Found) TexturesFound++;
            ImgCache.emplace(ImgName, Found);
            return Found;
        };

        // T6's MaterialTextureDef points at its GfxImage directly (`.image`), no `.u` union wrapping it
        // like T5's `.u.image`.
        auto ColorMapName = [](const T6::Material* Mat) -> const char* {
            if (!Mat || !Mat->textureTable) return nullptr;
            for (unsigned i = 0; i < Mat->textureCount; i++)
            {
                const T6::MaterialTextureDef& Td = Mat->textureTable[i];
                if (Td.nameHash == 0xA0AB1041u && Td.image && Td.image->name) // colorMap, same hash as T5
                    return Td.image->name;
            }
            return nullptr;
        };

        // --- world mesh: one surface per material from the decode above, same shape as T5's M1 ---
        Maps::Mesh World;
        World.Name = "world";
        int WorldMatIdx = 0;
        size_t WorldTextured = 0;
        for (auto& [Mat, G] : WorldGroups)
        {
            const int Fallback = WorldMatIdx++;
            if (G.verts.empty() || G.idx.empty()) continue;
            const std::string Nm = (Mat && Mat->info.name) ? std::string(Mat->info.name)
                                                            : ("surf_mat_" + std::to_string(Fallback));
            const char* Cm = ColorMapName(Mat);
            if (ResolveTextureByName(Cm ? Cm : std::string())) WorldTextured++;
            AddOatMaterial(Scene, Nm, Cm ? Cm : "");
            World.Surfaces.push_back(OatSurface(Nm, G.verts, G.idx));
        }
        const size_t WorldMeshes = World.Surfaces.size();
        if (!World.Surfaces.empty()) Scene.Objects.push_back(std::move(World));

        // --- props: instanced static models, same approach as T5's M3 ---
        xmodel::ToCommonConverter* Converter = xmodel::ToCommonConverter::GetForGame(GameId::T6);
        std::unordered_map<const void*, const XAssetInfoGeneric*> ModelInfo;
        for (auto* Info : Z.m_pools.PoolAssets<T6::AssetXModel>())
            if (Info && Info->Asset())
                ModelInfo.emplace((const void*)Info->Asset(), Info);

        // Shared by props AND the skybox mesh below: one XModel (OAT's own converter, LOD 0) as a
        // scene model, textured through ResolveTextureByName. The model's name, "" without geometry.
        auto ConvertModel = [&](const XAssetInfoGeneric& Info, const char* FallbackMatName) -> std::string {
            const auto Common = Converter->Convert(Info, 0);
            return OatModel(Scene, Common ? &*Common : nullptr, FallbackMatName,
                            [&](const std::string& N) { return ResolveTextureByName(N); });
        };

        std::unordered_map<const void*, std::string> ModelMeshCache;
        size_t PropInstances = 0;

        if (Converter && Gw->dpvs.smodelDrawInsts)
        {
            for (unsigned i = 0; i < Gw->dpvs.smodelCount; i++)
            {
                const T6::GfxStaticModelDrawInst& Inst = Gw->dpvs.smodelDrawInsts[i];
                if (!Inst.model)
                    continue;
                const void* Key = Inst.model;

                auto CacheIt = ModelMeshCache.find(Key);
                if (CacheIt == ModelMeshCache.end())
                {
                    std::string Name;
                    auto InfoIt = ModelInfo.find(Key);
                    if (InfoIt != ModelInfo.end())
                        Name = ConvertModel(*InfoIt->second, "prop_mat");
                    CacheIt = ModelMeshCache.emplace(Key, std::move(Name)).first;
                }

                if (CacheIt->second.empty())
                    continue;
                const T6::GfxPackedPlacement& Pl = Inst.placement;
                Scene.StaticInstances.push_back(OatInstance("prop_" + std::to_string(i), CacheIt->second,
                                                            AxisFloats9(Pl), Vec3Floats(Pl.origin), Pl.scale));
                PropInstances++;
            }
        }

        // --- skybox: T6's sky is a real XModel (skyBoxModel), converted and placed at the origin,
        //     same machinery as props - unlike T5's flat-texture-plus-Blender-script approach.
        bool SkyFound = false;
        if (Converter && Gw->skyBoxModel && Gw->skyBoxModel[0])
        {
            const XAssetInfoGeneric* SkyInfo = nullptr;
            for (auto* Info : Z.m_pools.PoolAssets<T6::AssetXModel>())
            {
                const T6::XModel* M = Info ? Info->Asset() : nullptr;
                if (M && M->name && std::strcmp(M->name, Gw->skyBoxModel) == 0) { SkyInfo = Info; break; }
            }
            if (SkyInfo)
            {
                SkyFound = true;
                Scene.Sky.Model = ConvertModel(*SkyInfo, "sky_mat");
            }
        }

        // --- lights: ComWorld primary lights, same approach as T5's M4 ---
        const T6::ComWorld* Cw = nullptr;
        for (auto* Info : Z.m_pools.PoolAssets<T6::AssetComWorld>())
            if (Info && Info->Asset()) { Cw = Info->Asset(); break; }
        if (Cw && Cw->primaryLights)
            for (unsigned i = 0; i < Cw->primaryLightCount; i++)
            {
                const T6::ComPrimaryLight& L = Cw->primaryLights[i];
                Scene.Lights.push_back(OatLight((unsigned char)L.type, Vec3Floats(L.origin), Vec3Floats(L.dir),
                                                Vec3Floats(L.color), L.radius, L.cosHalfFovOuter));
            }

        Maps::WriteStats Stats;
        std::string Error;
        const bool Ok = Maps::WriteGlb(Scene, OutGlbPath, nullptr, Stats, Error);
        if (EmbedTextures)
            std::filesystem::remove_all(TexTmp, Ec); // textures are embedded in the GLB; drop the temp files

        std::ostringstream Os;
        Os << "map=" << (Gw->name ? Gw->name : "?") << "\n";
        if (WorldMeshOk)
        {
            Os << "world_vertex_stride=" << VertexStride << "\n";
            Os << "world_indices=" << IC << "\n";
            Os << "world_surfaces=" << Gw->surfaceCount << "\n";
            Os << "world_materials=" << WorldGroups.size() << "\n";
            Os << "world_meshes=" << WorldMeshes << "\n";
            Os << "world_surfaces_textured=" << WorldTextured << "\n";
            Os << "world_triangles=" << WorldTris << "\n";
        }
        else
        {
            Os << "world_mesh=unsupported (" << WorldMeshNote << ")\n";
        }
        Os << "textures_embedded=" << TexturesFound << " (" << ImgCache.size() << " unique names looked up)\n";
        if (!CommonZonesTried.empty())
        {
            Os << "common_zones_scanned_for_textures=";
            for (size_t i = 0; i < CommonZonesTried.size(); i++)
                Os << (i ? ", " : "") << CommonZonesTried[i];
            Os << "\n";
        }
        Os << "prop_instances=" << PropInstances << "\n";
        Os << "prop_unique_models=" << ModelMeshCache.size() << "\n";
        Os << "prop_object_nodes=" << Stats.InstanceNodes << "\n";
        Os << "lights=" << Stats.Lights << "\n";
        Os << "skybox=" << (SkyFound ? (std::to_string(Stats.SkyMeshes) + " mesh(es) from '" +
                                        std::string(Gw->skyBoxModel) + "'")
                                     : (Gw->skyBoxModel && Gw->skyBoxModel[0]
                                            ? (std::string("model '") + Gw->skyBoxModel + "' not found in zone")
                                            : std::string("none (no skyBoxModel on this GfxWorld)"))) << "\n";
        Os << Maps::Describe(Stats);
        Os << "out=" << OutGlbPath << "\n";
        Os << "glb=" << (Ok ? "ok" : "failed " + Error) << "\n";
        Report = Os.str();
        return Ok;
    }
}

static const std::unordered_map<std::string, std::string>& GetOursMechanismMap()
{
    static const std::unordered_map<std::string, std::string> Map =
    {
        { "gfxworld", "ours: map glb" },
        { "comworld", "ours: map glb (lights)" },
        { "clipmap", "ours: clipmap capture" },
        { "clipmap_pvs", "ours: clipmap capture" },
        { "rawfile", "ours: rawfile dump" },
        { "soundbank", "ours: sound bank" },
        { "sound_patch", "ours: sound patch" },
        { "destructibledef", "ours: json" },
        { "xglobals", "ours: json" },
        { "glasses", "ours: json" },
        { "emblemset", "ours: json" },
        { "ddl", "ours: json" },
        { "keyvaluepairs", "ours: json" },
        { "menu", "ours: json" },
        { "menulist", "ours: json" },
        { "packindex", "ours: json" },
        { "snddriver_globals", "ours: json" },
        { "gameworldsp", "ours: json" },
        { "gameworldmp", "ours: json" },
        { "fximpacttable", "ours: json" },
        { "fx", "ours: json" }
    };
    return Map;
}

bool OatDescribeZone(const std::string& FastFilePath, OatZoneListing& Out, std::string& Error)
{
    auto ZonePtr = LoadZoneOrReport(FastFilePath, Error);
    if (!ZonePtr)
    {
        return false;
    }

    Zone* Z = ZonePtr.get();
    const IGame* GameInfo = IGame::GetGameById(Z->m_game_id);
    if (!GameInfo)
    {
        Error = "game info not found for zone";
        return false;
    }

    Out.Game = GameInfo->GetShortName();
    Out.Zone = Z->m_name;

    for (auto It = Z->m_pools.begin(); It != Z->m_pools.end(); ++It)
    {
        if (!*It)
        {
            continue;
        }

        OatZoneListing::Entry Entry;
        Entry.TypeId = static_cast<uint32_t>((*It)->m_type);
        Entry.IsReference = (*It)->IsReference();

        auto NameOpt = GameInfo->GetAssetTypeName((*It)->m_type);
        if (NameOpt)
        {
            Entry.TypeName = *NameOpt;
        }

        Entry.Name = (*It)->m_name;
        Out.Assets.push_back(std::move(Entry));
    }

    // Registering the dumpers needs a dumping context, and a context needs somewhere to write. This
    // asks what the writers handle without dumping anything, so the paths point at a scratch
    // directory that is never written to.
    IObjWriter* Writer = IObjWriter::GetObjWriterForGame(Z->m_game_id);
    if (Writer)
    {
        SharedSearchPaths Paths;
        const std::string ScratchDir = (std::filesystem::temp_directory_path() / "greyhound_coverage").string();
        OutputPathFilesystem OutputPath(ScratchDir);
        AssetDumpingContext Context(*Z, ScratchDir, OutputPath, Paths.GetSearchPath(), std::nullopt);
        std::vector<asset_type_t> HandledTypes = Writer->HandledAssetTypes(Context);
        for (asset_type_t Type : HandledTypes)
        {
            Out.ExtractableTypes.push_back(static_cast<uint32_t>(Type));
        }
    }

    return true;
}

bool OatZoneCoverage(const std::string& FastFilePath, std::string& Report)
{
    OatZoneListing Listing;
    std::string Error;
    if (!OatDescribeZone(FastFilePath, Listing, Error))
    {
        Report = Error;
        return false;
    }

    std::map<uint32_t, uint32_t> TypeCounts;
    uint32_t TotalAssets = 0;

    for (const auto& Asset : Listing.Assets)
    {
        // References point to assets in other zones, so they have no local content to extract
        if (Asset.IsReference)
        {
            continue;
        }

        ++TypeCounts[Asset.TypeId];
        ++TotalAssets;
    }

    std::unordered_set<uint32_t> OatHandledSet(Listing.ExtractableTypes.begin(), Listing.ExtractableTypes.end());
    const auto& OursMap = GetOursMechanismMap();

    struct TypeEntry
    {
        uint32_t Type;
        uint32_t Count;
        std::string Name;
        std::string Mechanism;
    };

    std::vector<TypeEntry> Entries;
    uint32_t CoveredTypes = 0;

    for (const auto& Pair : TypeCounts)
    {
        uint32_t Type = Pair.first;
        uint32_t Count = Pair.second;

        std::string TypeName;
        for (const auto& Asset : Listing.Assets)
        {
            if (Asset.TypeId == Type && !Asset.TypeName.empty())
            {
                TypeName = Asset.TypeName;
                break;
            }
        }

        std::string Mechanism = "none";
        if (OatHandledSet.count(Type) > 0)
        {
            Mechanism = "oat";
        }
        else
        {
            auto It = OursMap.find(TypeName);
            if (It != OursMap.end())
            {
                Mechanism = It->second;
            }
        }

        if (Mechanism != "none")
        {
            ++CoveredTypes;
        }

        Entries.push_back({ Type, Count, TypeName, Mechanism });
    }

    // Sort by descending count so the most significant types appear first
    std::sort(Entries.begin(), Entries.end(), [](const TypeEntry& A, const TypeEntry& B)
    {
        return A.Count > B.Count;
    });

    Report = "zone=" + Listing.Zone + "\n";
    Report += "game=" + Listing.Game + "\n";
    Report += "total_assets=" + std::to_string(TotalAssets) + "\n";
    Report += "covered=" + std::to_string(CoveredTypes) + "/" + std::to_string(Entries.size()) + "\n";

    for (const auto& Entry : Entries)
    {
        Report += "type=" + Entry.Name + " count=" + std::to_string(Entry.Count) + " via=" + Entry.Mechanism + "\n";
    }

    for (const auto& Entry : Entries)
    {
        if (Entry.Mechanism == "none")
        {
            Report += "uncovered=" + Entry.Name + " count=" + std::to_string(Entry.Count) + "\n";
        }
    }

    return true;
}

bool OatExportMapGlb(const std::string& FastFilePath, const std::string& OutGlbPath, std::string& Report,
                     bool EmbedTextures)
{
    std::unique_ptr<Zone> Z = LoadZoneOrReport(FastFilePath, Report);
    if (!Z)
        return false;
    if (Z->m_game_id == GameId::T6)
        return ExportMapGlbT6(*Z, FastFilePath, OutGlbPath, Report, EmbedTextures);
    if (Z->m_game_id != GameId::T5) { Report = "map export: only T5 (BO1) and T6 (BO2) supported for now"; return false; }

    // A map zone carries one GfxWorld (the static world). Grab the first.
    const T5::GfxWorld* Gw = nullptr;
    for (auto* Info : Z->m_pools.PoolAssets<T5::AssetGfxWorld>())
        if (Info && Info->Asset()) { Gw = Info->Asset(); break; }
    if (!Gw) { Report = "no GfxWorld in zone (not a map fastfile?)"; return false; }
    if (!Gw->draw.vd.vertices || Gw->draw.vertexCount == 0 || !Gw->draw.indices || !Gw->dpvs.surfaces)
    { Report = "GfxWorld has no static world mesh"; return false; }

    const T5::GfxWorldVertex* const V = Gw->draw.vd.vertices;
    const unsigned VC = Gw->draw.vertexCount;
    const uint16_t* const IB = Gw->draw.indices;
    const int IC = Gw->draw.indexCount;

    // Vertices stay in raw CoD world space (Z-up, inches); a single scene-root node converts the whole
    // scene to glTF (Y-up, metres) - so world mesh + instanced props share one transform.
    auto Decode = [&](unsigned gi, GltfSceneWriter::Vertex& out) -> bool {
        if (gi >= VC) return false;
        const T5::GfxWorldVertex& v = V[gi];
        out.pos[0] = v.xyz[0]; out.pos[1] = v.xyz[1]; out.pos[2] = v.xyz[2];
        float n[3] = {0, 0, 1};
        T5::Common::Vec3UnpackUnitVec(v.normal, n);
        out.normal[0] = n[0]; out.normal[1] = n[1]; out.normal[2] = n[2];
        out.uv[0] = v.texCoord[0]; out.uv[1] = v.texCoord[1];
        out.color[0] = (uint8_t)v.color.array[0]; out.color[1] = (uint8_t)v.color.array[1];
        out.color[2] = (uint8_t)v.color.array[2]; out.color[3] = (uint8_t)v.color.array[3];
        return true;
    };

    struct Accum {
        std::vector<GltfSceneWriter::Vertex> verts;
        std::vector<uint32_t> idx;
        std::unordered_map<unsigned, uint32_t> remap;
    };
    FirstSeenMap<const T5::Material*, Accum> Groups; // one glTF mesh per material

    size_t Tris = 0;
    for (int s = 0; s < Gw->surfaceCount; s++)
    {
        const T5::GfxSurface& Sf = Gw->dpvs.surfaces[s];
        Accum& G = Groups[Sf.material];
        const int base = Sf.tris.baseIndex;
        const unsigned fv = (unsigned)Sf.tris.firstVertex;

        for (unsigned t = 0; t < Sf.tris.triCount; t++)
        {
            const int i0 = base + (int)t * 3;
            if (i0 < 0 || i0 + 2 >= IC) break;
            static const int Order[3] = {0, 1, 2}; // the game's winding: the map writer reverses it
            uint32_t tri[3];
            bool ok = true;
            for (int k = 0; k < 3; k++)
            {
                const unsigned gi = fv + IB[i0 + Order[k]];
                auto it = G.remap.find(gi);
                if (it != G.remap.end()) { tri[k] = it->second; continue; }
                GltfSceneWriter::Vertex ov;
                if (!Decode(gi, ov)) { ok = false; break; }
                const uint32_t local = (uint32_t)G.verts.size();
                G.verts.push_back(ov);
                G.remap.emplace(gi, local);
                tri[k] = local;
            }
            if (ok) { G.idx.push_back(tri[0]); G.idx.push_back(tri[1]); G.idx.push_back(tri[2]); Tris++; }
        }
    }

    // M2 - materials + textures. Resolve each world material's colorMap (nameHash 0xA0AB1041) to its
    // GfxImage name, extract the IWi from the install's IWDs to a PNG, and embed it in the GLB.
    std::error_code Ec;
    const std::string InstallDir = FindInstallDir(FastFilePath);
    const std::filesystem::path TexTmp =
        std::filesystem::temp_directory_path() / ("gxmaptex_" + std::filesystem::path(OutGlbPath).stem().string());
    std::filesystem::create_directories(TexTmp, Ec);

    Maps::MapScene Scene;
    Scene.Name = Gw->name ? Gw->name : "";
    Scene.Game = "bo1";
    std::unordered_map<std::string, bool> ImgCache; // image name -> has pixels (dedup)

    auto ColorMapName = [](const T5::Material* Mat) -> const char* {
        if (!Mat || !Mat->textureTable) return nullptr;
        for (unsigned i = 0; i < Mat->textureCount; i++)
        {
            const T5::MaterialTextureDef& Td = Mat->textureTable[i];
            if (Td.nameHash == 0xA0AB1041u && Td.u.image && Td.u.image->name) // colorMap
                return Td.u.image->name;
        }
        return nullptr;
    };
    auto ResolveTextureByName = [&](const std::string& ImgName) -> bool {
        if (!EmbedTextures || InstallDir.empty() || ImgName.empty() || ImgName[0] == '$') // $white etc.
            return false;
        auto It = ImgCache.find(ImgName);
        if (It != ImgCache.end())
            return It->second;
        const std::filesystem::path Png = TexTmp / (ImgName + ".png");
        const bool Found = ExtractIwiToPng(InstallDir, ImgName, Png.string()) && ReadPng(Png, Scene.Images[ImgName].Png);
        ImgCache.emplace(ImgName, Found);
        return Found;
    };

    // --- world mesh: one surface per material ---
    Maps::Mesh World;
    World.Name = "world";
    int matIdx = 0;
    size_t Textured = 0;
    for (auto& [Mat, G] : Groups)
    {
        const int fallback = matIdx++;
        if (G.verts.empty() || G.idx.empty()) continue;
        const std::string Nm = (Mat && Mat->info.name) ? std::string(Mat->info.name)
                                                        : ("surf_mat_" + std::to_string(fallback));
        const char* Cm = ColorMapName(Mat);
        if (ResolveTextureByName(Cm ? Cm : std::string())) Textured++;
        AddOatMaterial(Scene, Nm, Cm ? Cm : "");
        World.Surfaces.push_back(OatSurface(Nm, G.verts, G.idx));
    }
    const size_t Meshes = World.Surfaces.size();
    if (!World.Surfaces.empty()) Scene.Objects.push_back(std::move(World));

    // --- M3: static-model props, instanced. Each unique XModel is converted once (OAT's converter,
    //     LOD 0) into a scene model; every placement is an instance of it (shared geometry). ---
    xmodel::ToCommonConverter* Converter = xmodel::ToCommonConverter::GetForGame(GameId::T5);
    std::unordered_map<const void*, const XAssetInfoGeneric*> ModelInfo;
    for (auto* Info : Z->m_pools.PoolAssets<T5::AssetXModel>())
        if (Info && Info->Asset())
            ModelInfo.emplace((const void*)Info->Asset(), Info);

    std::unordered_map<const void*, std::string> ModelMeshCache; // XModel* -> scene model name
    size_t PropInstances = 0;

    if (Converter && Gw->dpvs.smodelDrawInsts)
    {
        for (unsigned i = 0; i < Gw->dpvs.smodelCount; i++)
        {
            const T5::GfxStaticModelDrawInst& Inst = Gw->dpvs.smodelDrawInsts[i];
            if (!Inst.model)
                continue;
            const void* Key = Inst.model;

            auto CacheIt = ModelMeshCache.find(Key);
            if (CacheIt == ModelMeshCache.end())
            {
                std::string Name;
                auto InfoIt = ModelInfo.find(Key);
                if (InfoIt != ModelInfo.end())
                {
                    const auto Common = Converter->Convert(*InfoIt->second, 0);
                    Name = OatModel(Scene, Common ? &*Common : nullptr, "prop_mat",
                                    [&](const std::string& N) { return ResolveTextureByName(N); });
                }
                CacheIt = ModelMeshCache.emplace(Key, std::move(Name)).first;
            }

            if (CacheIt->second.empty())
                continue;
            const T5::GfxPackedPlacement& Pl = Inst.placement;
            Scene.StaticInstances.push_back(OatInstance("prop_" + std::to_string(i), CacheIt->second,
                                                        &Pl.axis[0][0], Pl.origin, Pl.scale));
            PropInstances++;
        }
    }

    // --- M4: lights. GfxWorld carries only the sun; the full point/spot array is in ComWorld. ---
    const T5::ComWorld* Cw = nullptr;
    for (auto* Info : Z->m_pools.PoolAssets<T5::AssetComWorld>())
        if (Info && Info->Asset()) { Cw = Info->Asset(); break; }
    if (Cw && Cw->primaryLights)
        for (unsigned i = 0; i < Cw->primaryLightCount; i++)
        {
            const T5::ComPrimaryLight& L = Cw->primaryLights[i];
            Scene.Lights.push_back(OatLight((unsigned char)L.type, L.origin, L.dir, L.color, L.radius, L.cosHalfFovOuter));
        }

    // --- M5: skybox. glTF has no native sky: the sky texture goes beside the GLB and the writer's
    //     Blender script wires it as the world background. ---
    std::filesystem::create_directories(std::filesystem::path(OutGlbPath).parent_path(), Ec);
    bool SkyPng = false;
    if (Gw->skyImage && Gw->skyImage->name && Gw->skyImage->name[0] != '$')
    {
        Scene.Sky.Image = Gw->skyImage->name;
        const std::filesystem::path Png = TexTmp / "sky.png";
        if (EmbedTextures && !InstallDir.empty() && ExtractIwiToPng(InstallDir, Gw->skyImage->name, Png.string()))
            SkyPng = ReadPng(Png, Scene.Images[Scene.Sky.Image].Png);
    }

    Maps::WriteStats Stats;
    std::string Error;
    const bool Ok = Maps::WriteGlb(Scene, OutGlbPath, nullptr, Stats, Error);
    std::filesystem::remove_all(TexTmp, Ec); // textures are embedded; drop the temp PNGs

    const std::string Stem = std::filesystem::path(OutGlbPath).stem().string();
    std::ostringstream Os;
    Os << "map=" << (Gw->name ? Gw->name : "?") << "\n";
    Os << "world_vertices=" << VC << "\n";
    Os << "world_indices=" << IC << "\n";
    Os << "surfaces=" << Gw->surfaceCount << "\n";
    Os << "materials=" << Groups.size() << "\n";
    Os << "meshes=" << Meshes << "\n";
    Os << "textures_embedded=" << ImgCache.size() << "\n";
    Os << "surfaces_textured=" << Textured << "\n";
    Os << "triangles=" << Tris << "\n";
    Os << "prop_instances=" << PropInstances << "\n";
    Os << "prop_unique_models=" << ModelMeshCache.size() << "\n";
    Os << "prop_object_nodes=" << Stats.InstanceNodes << "\n";
    Os << "lights=" << Stats.Lights << "\n";
    Os << "skybox=" << (SkyPng ? (Stem + "_sky.png") : std::string("none")) << "\n";
    Os << "blender_script=" << (Stem + "_import.py") << "\n";
    Os << Maps::Describe(Stats);
    Os << "out=" << OutGlbPath << "\n";
    Os << "glb=" << (Ok ? "ok" : "failed " + Error) << "\n";
    Report = Os.str();
    return Ok;
}
