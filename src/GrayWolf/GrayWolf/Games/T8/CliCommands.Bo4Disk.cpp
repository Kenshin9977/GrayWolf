// CliCommands.Bo4Disk.cpp - `bo4-disk-export`: BO4 zones walked with the zonekit tables
// (Bo4DiskExport), their asset list reported and their in-zone images written, no game needed.
#include "stdafx.h"
#include "Games/Treyarch/TreyarchPackages.h"
#include "XPAKCache.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include "Bo4DiskExport.h"
#include "CoDPackageCache.h"
#include "CliCommands.h"
#include "CliSupport.h"

namespace fs = std::filesystem;

// bo4-zone-diag <zones> [--type strings|matimages|refs|imagekeys] [--out dir]: what the BO4 walk
// finds, per zone, for diagnosis. Exporting assets is disk-export (bo4-disk-export), DiskGame.h.
int RunBo4ZoneDiag(const CliOptions& Options, const std::string& Input)
{
    const std::string Profile = Options.ZoneWalkProfile.empty() ? "bo4" : Options.ZoneWalkProfile;
    const bool Export = !Options.Out.empty();
    const bool StringsOnly = Options.Type == "strings";   // <out>/<zone>.strings.txt: index, text
    const bool MatImages = Options.Type == "matimages";  // <out>/<zone>.matimages.csv
    const bool Refs = Options.Type == "refs";            // <out>/<zone>.refs.csv
    // <out>/<zone>.imagekeys.csv: name hash, DXGI format, width, height, streamed, in-zone bytes, mip keys
    const bool ImageKeys = Options.Type == "imagekeys";
    if (Export && !StringsOnly && !MatImages && !Refs && !ImageKeys)
    {
        CliReport(Options, "result=error\nreason=bo4-zone-diag --type strings|matimages|refs|imagekeys; assets: disk-export --game bo4\n");
        return 1;
    }
    const auto Packages = Options.Install.empty() ? nullptr : TreyarchPackages::Open<XPAKCache>(Options.Install);
    const std::string CacheDir = !Options.Cache.empty() ? Options.Cache : (fs::temp_directory_path() / "greyhound_bo4ff").string();
    const std::string UiRes = Options.UiRes.empty() ? "4k" : Options.UiRes;
    std::vector<std::string> Zones;
    size_t SkippedUiRes = 0;
    std::string Error;
    if (!Bo4DiskExport::Zones(Input, Packages.get(), CacheDir, UiRes, Zones, SkippedUiRes, Error))
    {
        CliReport(Options, "result=error\nreason=" + Error + "\n");
        return 5;
    }

    std::ostringstream Lines;
    std::map<std::string, size_t> TypeCounts;
    size_t Complete = 0, Failed = 0, Images = 0;
    for (const auto& Path : Zones)
    {
        const std::string Stem = fs::path(Path).stem().string();
        ZoneBytes Zone;
        Bo4DiskExport::ZoneAssets A;
        if (!Bo4DiskExport::LoadZone(Path, Zone, Error) || !Bo4DiskExport::ReadZone(Zone, Profile, A, Error))
        {
            Failed++;
            Lines << "zone=" << Stem << " error=" << Error << "\n";
            continue;
        }
        std::error_code Ec;
        if (Export) fs::create_directories(Options.Out, Ec);
        if (Export && Refs)
        {
            std::ofstream F(fs::path(Options.Out) / (Stem + ".refs.csv"), std::ios::binary);
            F << std::hex;
            for (const auto& R : Bo4DiskExport::AssetRefs(A))
                F << R.OwnerType << "," << R.Owner << "," << R.TargetType << "," << R.Target << "\n";
            std::ofstream G(fs::path(Options.Out) / (Stem + ".roots.csv"), std::ios::binary);
            G << std::hex;
            for (const auto& [Type, Pos] : A.Assets) G << Type << "," << Pos << "\n";
        }
        else if (Export && ImageKeys)
        {
            std::ofstream F(fs::path(Options.Out) / (Stem + ".imagekeys.csv"), std::ios::binary);
            for (const auto& Img : A.Images)
            {
                F << std::hex << Img.Hash << std::dec << "," << Img.Format << "," << Img.Width << "," << Img.Height
                  << "," << (Img.Streamed ? 1 : 0) << "," << (Img.PayloadPos >= 0 ? Img.PayloadSize : 0) << ",";
                for (size_t K = 0; K < Img.MipKeys.size(); K++) F << (K ? ";" : "") << std::hex << Img.MipKeys[K] << std::dec;
                F << "\n";
            }
        }
        else if (Export && MatImages)
        {
            std::ofstream F(fs::path(Options.Out) / (Stem + ".matimages.csv"), std::ios::binary);
            F << std::hex;
            for (const auto& M : Bo4DiskExport::MaterialImages(Zone, A))
                F << M.Material << "," << M.Slot << "," << M.Semantic << "," << M.Image << "\n";
        }
        else if (Export && StringsOnly)
        {
            std::ofstream F(fs::path(Options.Out) / (Stem + ".strings.txt"), std::ios::binary);
            const auto& Table = Bo4DiskExport::ZoneStringTable(Zone);
            for (size_t K = 0; K < Table.size(); K++) F << K << "\t" << Table[K] << "\n";
        }
        Complete += A.Complete ? 1 : 0;
        for (const auto& Asset : A.Assets) TypeCounts[Asset.first]++;
        Images += A.Images.size();
        Lines << "zone=" << Stem << " assets=" << A.Assets.size() << " images=" << A.Images.size()
              << " models=" << A.Models.size() << " anims=" << A.Anims.size() << " backrefs=" << A.BackRefsResolved
              << "/" << A.BackRefs << " walk=" << A.Why << "\n";
    }
    std::ostringstream R;
    R << "result=ok\nzones=" << Zones.size() << "\nui_res=" << UiRes << "\nzones_skipped_ui_res=" << SkippedUiRes
      << "\nzones_walked_to_end=" << Complete << "\nzones_failed=" << Failed << "\nimages=" << Images << "\n";
    for (const auto& [Type, N] : TypeCounts) R << "type." << Type << "=" << N << "\n";
    R << Lines.str();
    CliReport(Options, R.str());
    return Failed == Zones.size() && !Zones.empty() ? 5 : 0;
}
