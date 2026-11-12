#include "stdafx.h"

#include "MaterialSplit.h"

#include "DiskGame.h"
#include "ExportNaming.h"
#include "ExportWriters.h"   // ImageFormatSetting

#include "DirectXTex.h"
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace fs = std::filesystem;

namespace MaterialSplit
{
namespace
{
    // The role an image is read in, and what its standard file takes of it
    struct Read
    {
        std::string Role;
        Output Out;
    };

    Output Grey(const char* Suffix, const Materials::Map& M)
    {
        Output O;
        O.Suffix = Suffix;
        O.What = Output::Op::Grey;
        O.Channel = M.Channel & 3;
        O.Invert = M.Invert;
        return O;
    }
    Output Rgb(const char* Suffix)
    {
        Output O;
        O.Suffix = Suffix;
        O.What = Output::Op::Rgb;
        return O;
    }

    // every map a description reads, with the role it is read in
    void Reads(const Materials::Description& D, std::map<std::string, std::vector<Read>>& Out)
    {
        const auto Put = [&](const char* Role, const Materials::Map& M, Output O) {
            if (!M.Empty()) Out[M.Path].push_back({ Role, std::move(O) });
        };
        Put("albedo", D.Albedo, Rgb("_albedo"));
        if (!D.Normal.Empty())
        {
            Output N;
            N.Suffix = "_normal";
            N.What = Output::Op::Normal;
            N.Enc = D.NormalEnc;
            Put(D.NormalEnc == Materials::NormalEncoding::AG ? "normal_ag" : "normal", D.Normal, N);
        }
        // a gloss read inverted is the roughness the file is named for
        Put("roughness", D.Roughness, Grey("_roughness", D.Roughness));
        Put("metalness", D.Metalness, Grey("_metalness", D.Metalness));
        Put("specular", D.Specular, Rgb("_specular"));
        Put("occlusion", D.Occlusion, Grey("_ao", D.Occlusion));
        Put("emissive", D.Emissive, Rgb("_emissive"));
        // a transmission read inverted is a thickness map: written as it is stored
        Put("transmission", D.Transmission.Image, Grey("_thickness", { D.Transmission.Image.Path, D.Transmission.Image.Channel, false }));
        Put("opacity", D.AlphaMap.Image, Grey("_opacity", D.AlphaMap.Image));
        for (const Materials::Layer& L : D.Layers)
        {
            Put("albedo", L.Albedo, Rgb("_albedo"));
            Put("roughness", L.Roughness, Grey("_roughness", L.Roughness));
        }
    }

    // An image as 8-bit RGBA: PNG and TGA through stb, DDS through DirectXTex (its first level)
    bool LoadRgba(const fs::path& File, int& W, int& H, std::vector<uint8_t>& Px)
    {
        const std::string Ext = ExportNaming::Lower(File.extension().string());
        if (Ext != ".dds")
        {
            int N = 0;
            stbi_uc* P = stbi_load(File.string().c_str(), &W, &H, &N, 4);
            if (P == nullptr) return false;
            Px.assign(P, P + (size_t)W * H * 4);
            stbi_image_free(P);
            return true;
        }
        namespace Dx = DirectX;
        Dx::TexMetadata M;
        Dx::ScratchImage Img;
        if (FAILED(Dx::LoadFromDDSFile(File.wstring().c_str(), Dx::DDS_FLAGS_NONE, &M, Img))) return false;
        const Dx::Image* Top = Img.GetImage(0, 0, 0);
        if (Top == nullptr) return false;
        // decoded as stored: an sRGB format's bytes kept as they are, not converted to linear
        const DXGI_FORMAT Want = Dx::IsSRGB(M.format) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        Dx::ScratchImage Plain;
        const HRESULT Hr = Dx::IsCompressed(M.format) ? Dx::Decompress(*Top, Want, Plain)
                         : M.format == Want ? Plain.InitializeFromImage(*Top)
                                            : Dx::Convert(*Top, Want, Dx::TEX_FILTER_DEFAULT, Dx::TEX_THRESHOLD_DEFAULT, Plain);
        if (FAILED(Hr) || Plain.GetImage(0, 0, 0) == nullptr) return false;
        const Dx::Image& P = *Plain.GetImage(0, 0, 0);
        W = (int)P.width;
        H = (int)P.height;
        Px.resize((size_t)W * H * 4);
        for (int Y = 0; Y < H; Y++) std::copy_n(P.pixels + (size_t)Y * P.rowPitch, (size_t)W * 4, Px.data() + (size_t)Y * W * 4);
        return true;
    }

    bool Save(const fs::path& To, int W, int H, int Channels, const std::vector<uint8_t>& Px)
    {
        const std::string S = To.string();
        return ExportNaming::Lower(To.extension().string()) == ".tga" ? stbi_write_tga(S.c_str(), W, H, Channels, Px.data()) != 0
                                                                      : stbi_write_png(S.c_str(), W, H, Channels, Px.data(), W * Channels) != 0;
    }

    // One output of an RGBA source
    std::vector<uint8_t> Make(const Output& O, const std::vector<uint8_t>& Src, size_t Count, int& Channels)
    {
        Channels = O.What == Output::Op::Grey ? 1 : 3;
        std::vector<uint8_t> Out(Count * Channels);
        for (size_t I = 0; I < Count; I++)
        {
            const uint8_t* P = Src.data() + I * 4;
            uint8_t* Q = Out.data() + I * Channels;
            switch (O.What)
            {
            case Output::Op::Grey:
                Q[0] = O.Invert ? (uint8_t)(255 - P[O.Channel]) : P[O.Channel];
                break;
            case Output::Op::Rgb:
                Q[0] = P[0], Q[1] = P[1], Q[2] = P[2];
                break;
            case Output::Op::Normal:
            {
                // x, y as stored (the game's convention: no y flip), z rebuilt, as "patchnormals" writes them
                const uint8_t Xb = O.Enc == Materials::NormalEncoding::AG ? P[3] : P[0];
                const float X = Xb / 127.5f - 1.0f, Y = P[1] / 127.5f - 1.0f;
                const float Z = std::sqrt(std::max(0.0f, 1.0f - X * X - Y * Y));
                Q[0] = Xb;
                Q[1] = P[1];
                Q[2] = O.Enc == Materials::NormalEncoding::XYZ ? P[2] : (uint8_t)std::clamp(std::lround((Z + 1.0f) * 127.5f), 0L, 255L);
                break;
            }
            }
        }
        return Out;
    }

    // the outputs' extension: the export's image format, PNG for DDS
    std::string OutExtension()
    {
        return ExportNaming::Lower(ExportWriters::ImageFormatSetting()) == "tga" ? ".tga" : ".png";
    }

    bool Fresh(const fs::path& To, const fs::path& From)
    {
        std::error_code Ec, Ec2;
        return fs::is_regular_file(To, Ec) && fs::last_write_time(To, Ec) >= fs::last_write_time(From, Ec2) && !Ec && !Ec2;
    }
}

std::vector<std::pair<std::string, std::vector<Output>>> Plan(const std::vector<Materials::Description>& Ds)
{
    std::map<std::string, std::vector<Read>> All;
    for (const auto& D : Ds) Reads(D, All);
    std::vector<std::pair<std::string, std::vector<Output>>> Out;
    for (auto& [Path, List] : All)
    {
        std::set<std::string> Roles;
        for (const Read& R : List) Roles.insert(R.Role);
        // packed: read in two roles, or a normal with x in alpha (no other tool reads one)
        if (Roles.size() < 2 && !Roles.count("normal_ag")) continue;
        // an engine placeholder ("$white") is no texture of the asset's
        if (const std::string Stem = fs::path(Path).stem().string(); !Stem.empty() && Stem[0] == '$') continue;
        std::set<Output> Outputs;
        std::set<std::string> Suffixes;
        for (const Read& R : List)
            if (Suffixes.insert(R.Out.Suffix).second) Outputs.insert(R.Out);   // the first read of a role wins
        Out.emplace_back(Path, std::vector<Output>(Outputs.begin(), Outputs.end()));
    }
    return Out;
}

void WriteForRun(const DiskGame::Context& C, DiskGame::Totals& T)
{
    const fs::path Root(C.OutRoot);
    // every packed image of the run once: its absolute path -> what it splits into
    std::map<fs::path, std::set<Output>> Jobs;
    std::error_code Ec;
    for (fs::recursive_directory_iterator It(Root, Ec), End; !Ec && It != End; It.increment(Ec))
    {
        const std::string Name = It->path().filename().string();
        if (!It->is_regular_file(Ec) || Name.size() <= 15 || Name.compare(Name.size() - 15, 15, ".materials.json") != 0) continue;
        Materials::Sidecar Side;
        if (!Materials::ReadSidecar(It->path().string(), Side))
        {
            T.Counters["split_maps_sidecars_unread"]++;
            continue;
        }
        for (auto& [Path, Outputs] : Plan(Side.Materials))
        {
            auto& Job = Jobs[(It->path().parent_path() / fs::path(Path)).lexically_normal()];
            Job.insert(Outputs.begin(), Outputs.end());
        }
    }
    if (Jobs.empty()) return;

    const std::vector<std::pair<fs::path, std::set<Output>>> List(Jobs.begin(), Jobs.end());
    const std::string Ext = OutExtension();
    std::atomic<size_t> Next{ 0 }, Done{ 0 }, Files{ 0 }, Unread{ 0 }, Unwritten{ 0 };
    const auto Work = [&] {
        for (size_t I; (I = Next++) < List.size() && !C.Cancelled(); Done++)
        {
            const auto& [From, Outputs] = List[I];
            std::vector<std::pair<fs::path, const Output*>> Todo;
            for (const Output& O : Outputs)
            {
                const fs::path To = fs::path(From).replace_filename(From.stem().string() + O.Suffix + Ext);
                if (!Fresh(To, From)) Todo.emplace_back(To, &O);
            }
            if (Todo.empty()) continue;
            int W = 0, H = 0;
            std::vector<uint8_t> Px;
            if (!LoadRgba(From, W, H, Px))
            {
                Unread++;
                continue;
            }
            for (const auto& [To, O] : Todo)
            {
                int Channels = 0;
                const std::vector<uint8_t> Out = Make(*O, Px, (size_t)W * H, Channels);
                if (Save(To, W, H, Channels, Out)) Files++;
                else Unwritten++;
            }
        }
    };
    const unsigned N = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> Pool;
    for (unsigned I = 0; I < N; I++) Pool.emplace_back(Work);
    // the phase on the progress while the workers run (one thread reports)
    while (Done < List.size() && !C.Cancelled())
    {
        C.Working("splitting packed maps", Done, List.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    for (std::thread& Th : Pool) Th.join();
    C.Working("");
    T.Counters["split_maps_images"] += List.size();
    if (Files) T.Counters["split_maps_files"] += Files;
    if (Unread) T.Counters["split_maps_unread"] += Unread;
    if (Unwritten) T.Counters["split_maps_unwritten"] += Unwritten;
}
}
