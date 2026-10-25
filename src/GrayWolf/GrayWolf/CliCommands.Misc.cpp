// NOMINMAX before <windows.h>: std::max/std::min below would be mangled by its macros.
#define NOMINMAX

#include <stdio.h>
#include <cmath>
#include <map>
#include <filesystem>
#include <algorithm>
#include <vector>
#include <iterator>
#include <functional>
#include <string>
#include <format>
#include <cstring>
#include <fstream>

#include "CliOptions.h"
#include "CliCommands.h"

#include <CLI/CLI.hpp>
#include "CliSupport.h"

#include "CoDAssets.h"
#include "FileSystems.h"
#include "Strings.h"
#include "BinaryWriter.h"
#include "FastFile.h"
#include "Games/GameModule.h"
#include "ZoneAssetTypes.h"
#include "ZonekitZone.h"

#include "XMEExport.h"
#include "XMBExport.h"
#include "XMEImport.h"
#include "Encryption.h"
#include "HuskyMath.h"
#include "GameImageProcessors.h"
#include "FBXWriter.h"
#include "WraithModel.h"
#include "Games/Common/MeshDecode.h"

// Assertions that need neither game files nor a window, so they can run anywhere.
// Returns the number of failures and appends each one to Failures.
static int SelfTest(std::string& Failures)
{
    int Failed = 0;
    const auto Check = [&](bool Condition, const char* Name, const std::string& Got)
    {
        if (!Condition)
        {
            Failed++;
            Failures += std::format("FAIL {} (got '{}')\n", Name, Got.c_str());
        }
    };

    // Trimming
    {
        std::string A = "  spaced  ";
        Check(Strings::Trim(A) == "spaced", "Strings::Trim", A);

        std::string B = "\t\r\n padded";
        Check(Strings::Trim(B) == "padded", "Strings::Trim leading", B);

        std::string C = "padded \t\r\n";
        Check(Strings::Trim(C) == "padded", "Strings::Trim trailing", C);

        std::string D = "   ";
        Check(Strings::Trim(D).empty(), "Strings::Trim all blank", D);

        std::string E = "";
        Check(Strings::Trim(E).empty(), "Strings::Trim empty", E);

        // High bytes must not reach std::isspace as negative chars, and must survive intact
        std::string F = " \xC3\xA9\xC3\xA0\xE4\xB8\xAD ";
        const auto FTrimmed = Strings::Trim(F);
        Check(FTrimmed == "\xC3\xA9\xC3\xA0\xE4\xB8\xAD", "Strings::Trim non-ascii", FTrimmed);

        std::string G = "\x80\xFF";
        const auto GTrimmed = Strings::Trim(G);
        Check(GTrimmed == "\x80\xFF", "Strings::Trim high bytes", GTrimmed);
    }

    // Path helpers, every export path is built with these
    {
        const auto A = FileSystems::GetExtension("a\\b\\c.XPAK");
        Check(A == ".XPAK", "FileSystems::GetExtension", A);

        const auto B = FileSystems::GetFileName("a\\b\\c.xpak");
        Check(B == "c.xpak", "FileSystems::GetFileName", B);

        const auto C = FileSystems::GetFileNameWithoutExtension("a\\b\\c.xpak");
        Check(C == "c", "FileSystems::GetFileNameWithoutExtension", C);

        const auto D = (std::filesystem::path("a\\b") / "c.xpak").string();
        Check(D == "a\\b\\c.xpak", "path join separator", D);

        const auto E = FileSystems::GetDirectoryName("a\\b\\c.xpak");
        Check(E == "a\\b", "FileSystems::GetDirectoryName", E);
    }

    // Case folding, the asset search lowercases every name it looks at
    {
        std::string A = "MiXeD";
        Check(Strings::ToLower(A) == "mixed", "Strings::ToLower", A);

        std::string B = "MiXeD";
        Check(Strings::ToUpper(B) == "MIXED", "Strings::ToUpper", B);
    }

    // Half floats, against the values binary16 defines.
    {
        struct { uint16_t Half; float Expected; const char* What; } Cases[] = {
            { 0x0000,  0.0f,      "zero" },
            { 0x8000, -0.0f,      "negative zero" },
            { 0x3C00,  1.0f,      "one" },
            { 0xBC00, -1.0f,      "minus one" },
            { 0x4000,  2.0f,      "two" },
            { 0x3800,  0.5f,      "half" },
            { 0xC500, -5.0f,      "minus five" },
            { 0x7BFF,  65504.0f,  "largest normal" },
            { 0x0400,  0.000061035156f, "smallest normal" },
            { 0x0001,  0.000000059604645f, "smallest subnormal" },
        };

        for (const auto& Case : Cases)
        {
            const auto Got = HuskyMath::HalfToFloat(Case.Half);
            const auto Tolerance = std::fabs(Case.Expected) * 1e-6f + 1e-12f;

            Check(std::fabs(Got - Case.Expected) <= Tolerance,
                std::format("HalfToFloat {}", Case.What).c_str(),
                std::format("{:g} expected {:g}", Got, Case.Expected));
        }

        // Ten bit normals span -1 to 1, so the extremes and the centre pin the mapping
        const auto Low = HuskyMath::UnpackNormalTenBit(0);
        Check(std::fabs(Low.X + 1.0f) < 0.001f && std::fabs(Low.Y + 1.0f) < 0.001f && std::fabs(Low.Z + 1.0f) < 0.001f,
            "UnpackNormalTenBit at zero", std::format("{:f} {:f} {:f}", Low.X, Low.Y, Low.Z));

        const uint32_t AllOnes = 0x3FF | (0x3FFu << 10) | (0x3FFu << 20);
        const auto High = HuskyMath::UnpackNormalTenBit(AllOnes);
        Check(std::fabs(High.X - 1.0f) < 0.001f && std::fabs(High.Y - 1.0f) < 0.001f && std::fabs(High.Z - 1.0f) < 0.001f,
            "UnpackNormalTenBit at maximum", std::format("{:f} {:f} {:f}", High.X, High.Y, High.Z));

        // A byte of 127 is the zero point of the scaled form, so all three axes collapse
        const uint32_t Centred = 127u | (127u << 8) | (127u << 16) | (200u << 24);
        const auto Middle = HuskyMath::UnpackNormalScaled(Centred);
        Check(std::fabs(Middle.X) < 0.0001f && std::fabs(Middle.Y) < 0.0001f && std::fabs(Middle.Z) < 0.0001f,
            "UnpackNormalScaled at its zero point", std::format("{:f} {:f} {:f}", Middle.X, Middle.Y, Middle.Z));

        // And a unit vector should come back with roughly unit length
        const uint32_t Unit = 255u | (127u << 8) | (127u << 16) | (0xFFu << 24);
        const auto Long = HuskyMath::UnpackNormalScaled(Unit);
        const auto Length = std::sqrt(Long.X * Long.X + Long.Y * Long.Y + Long.Z * Long.Z);
        Check(Length > 0.0f && Length < 2.0f, "UnpackNormalScaled stays in range",
            std::format("length {:f}", Length));
    }

    // Binary FBX. The format makes every record state the byte offset where it ends, and
    // a wrong one is the classic way to produce a file that looks fine and opens in
    // nothing. So the check is to write a model, then walk the record tree back and
    // require every end offset to land exactly where the record actually stops.
    {
        FBXModelInput Model;
        Model.Name = "selftest";
        Model.Materials.push_back("mtl_test");

        Model.Bones.resize(2);
        Model.Bones[0].Name = "root";
        Model.Bones[0].Parent = -1;
        Model.Bones[1].Name = "j_head";
        Model.Bones[1].Parent = 0;
        Model.Bones[1].Position[2] = 10.0;

        FBXModelInput::Mesh Mesh;
        Mesh.Name = "mesh0";
        Mesh.MaterialIndex = 0;
        Mesh.Vertices.resize(4);

        for (int v = 0; v < 4; v++)
        {
            Mesh.Vertices[v].Position[0] = (v & 1) ? 1.0 : 0.0;
            Mesh.Vertices[v].Position[1] = (v & 2) ? 1.0 : 0.0;
            Mesh.Vertices[v].Normal[2] = 1.0;
            Mesh.Vertices[v].UV[0] = (v & 1) ? 1.0 : 0.0;
            Mesh.Vertices[v].UV[1] = (v & 2) ? 1.0 : 0.0;
            Mesh.Vertices[v].Weights.emplace_back(v < 2 ? 0 : 1, 1.0);
        }

        Mesh.Indices = { 0, 1, 2, 1, 3, 2 };
        Model.Meshes.push_back(std::move(Mesh));

        char TempPath[MAX_PATH] = {};
        GetTempPathA(MAX_PATH, TempPath);
        const auto Path = std::string(TempPath) + "greyhound-selftest.fbx";

        Check(FBXWriter::Write(Path, Model), "binary FBX writes", Path.c_str());

        std::vector<uint8_t> File;
        {
            FILE* Raw = nullptr;
            if (fopen_s(&Raw, Path.c_str(), "rb") == 0 && Raw != nullptr)
            {
                fseek(Raw, 0, SEEK_END);
                File.resize((size_t)ftell(Raw));
                fseek(Raw, 0, SEEK_SET);
                if (fread(File.data(), 1, File.size(), Raw) != File.size())
                {
                    File.clear();
                }
                fclose(Raw);
            }
        }

        Check(File.size() > 64, "the FBX has content",
            std::format("{} bytes", (unsigned long long)File.size()).c_str());

        if (File.size() > 64)
        {
            Check(std::memcmp(File.data(), "Kaydara FBX Binary  ", 20) == 0, "FBX magic", "");

            uint32_t Version = 0;
            std::memcpy(&Version, File.data() + 23, sizeof(Version));
            Check(Version == 7400, "FBX version is 7400", std::format("{}", Version).c_str());

            // Walk every record, following the end offsets. A record whose declared end
            // does not match where its own contents finish is the failure this catches.
            uint32_t Records = 0;
            uint32_t Mismatches = 0;
            uint32_t Deepest = 0;

            const std::function<void(size_t, size_t, uint32_t)> Walk =
                [&](size_t At, size_t Limit, uint32_t Depth)
            {
                Deepest = std::max(Deepest, Depth);

                while (At + 13 <= Limit)
                {
                    uint32_t End = 0, Properties = 0, PropertyBytes = 0;
                    std::memcpy(&End, File.data() + At, 4);
                    std::memcpy(&Properties, File.data() + At + 4, 4);
                    std::memcpy(&PropertyBytes, File.data() + At + 8, 4);
                    const auto NameLength = File[At + 12];

                    // An all zero record closes the list
                    if (End == 0 && Properties == 0 && PropertyBytes == 0 && NameLength == 0)
                    {
                        return;
                    }

                    if (End > Limit || End <= At)
                    {
                        Mismatches++;
                        return;
                    }

                    Records++;

                    const auto ChildrenAt = At + 13 + NameLength + PropertyBytes;

                    if (ChildrenAt > End)
                    {
                        Mismatches++;
                        return;
                    }

                    // Anything left over past the properties has to be nested records
                    if (ChildrenAt < End)
                    {
                        Walk(ChildrenAt, End, Depth + 1);
                    }

                    At = End;
                }
            };

            Walk(27, File.size(), 0);

            Check(Mismatches == 0, "every FBX record ends where it says it does",
                std::format("{} bad of {}", Mismatches, Records).c_str());
            Check(Records > 20, "the FBX holds a real tree",
                std::format("{} records", Records).c_str());
            Check(Deepest >= 3, "the FBX tree nests",
                std::format("depth {}", Deepest).c_str());
        }

        remove(Path.c_str());
    }

    // GameImageUtil's per pixel processors. These are checked against what the maths says
    // a normal map is, not against the numbers the port happens to produce.
    {
        // A pixel encoding a normal pointing straight out: X and Y at zero, so Z takes the
        // whole unit length and comes back at 1, which is 1.0 in the channel
        ImagePixel Flat;
        Flat.A = 0.5f; Flat.G = 0.5f;
        const auto Straight = GameImageProcessors::UnpackNormalFromAlphaGreen(Flat);
        Check(std::fabs(Straight.R - 0.5f) < 1e-5f && std::fabs(Straight.G - 0.5f) < 1e-5f &&
              std::fabs(Straight.B - 1.0f) < 1e-5f,
            "normal map with no tilt points straight out",
            std::format("{:f} {:f} {:f}", Straight.R, Straight.G, Straight.B));

        // Every unpacked normal has to be a unit vector, which is the property that says
        // the reconstruction is right rather than merely plausible
        for (int i = 0; i <= 8; i++)
        {
            ImagePixel Tilted;
            Tilted.A = 0.5f + 0.05f * i;
            Tilted.G = 0.5f - 0.02f * i;

            const auto Normal = GameImageProcessors::UnpackNormalFromAlphaGreen(Tilted);
            const auto NX = Normal.R * 2.0f - 1.0f;
            const auto NY = Normal.G * 2.0f - 1.0f;
            const auto NZ = Normal.B * 2.0f - 1.0f;
            const auto Unit = std::sqrt(NX * NX + NY * NY + NZ * NZ);

            Check(std::fabs(Unit - 1.0f) < 1e-4f,
                std::format("unpacked normal {} is a unit vector", i).c_str(),
                std::format("length {:f}", Unit));
        }

        // The two forms differ only in which channel carried X
        ImagePixel Packed;
        Packed.R = 0.7f; Packed.G = 0.3f; Packed.B = 0.0f; Packed.A = 0.7f;
        const auto FromAlpha = GameImageProcessors::UnpackNormalFromAlphaGreen(Packed);
        const auto FromRed = GameImageProcessors::UnpackNormalFromRedGreen(Packed);
        Check(std::fabs(FromAlpha.R - FromRed.R) < 1e-6f && std::fabs(FromAlpha.B - FromRed.B) < 1e-6f,
            "both normal map forms agree when their X channels match", "");

        // A channel pair that already overreaches unit length leaves nothing for Z, and has
        // to come back flat rather than as the root of a negative number
        ImagePixel Overreaching;
        Overreaching.A = 1.0f; Overreaching.G = 1.0f;
        const auto NoZ = GameImageProcessors::UnpackNormalFromAlphaGreen(Overreaching);
        Check(NoZ.B == 0.0f && std::isfinite(NoZ.B), "a normal with no room left for Z comes back flat",
            std::format("{:f}", NoZ.B));

        // The packed normal, occlusion and gloss map hands back three images, and its
        // normal is normalised by construction
        ImagePixel Nog;
        Nog.R = 0.25f; Nog.G = 0.6f; Nog.B = 0.75f; Nog.A = 0.4f;
        ImagePixel Normal, Occlusion, Gloss;
        GameImageProcessors::UnpackNormalOcclusionGloss(Nog, Normal, Occlusion, Gloss);

        Check(std::fabs(Gloss.R - 0.25f) < 1e-6f && Gloss.R == Gloss.G && Gloss.G == Gloss.B,
            "gloss comes out of the red channel as grey", std::format("{:f}", Gloss.R));
        Check(std::fabs(Occlusion.R - 0.75f) < 1e-6f && Occlusion.R == Occlusion.B,
            "occlusion comes out of the blue channel as grey", std::format("{:f}", Occlusion.R));

        const auto MX = Normal.R * 2.0f - 1.0f;
        const auto MY = Normal.G * 2.0f - 1.0f;
        const auto MZ = Normal.B * 2.0f - 1.0f;
        const auto MLength = std::sqrt(MX * MX + MY * MY + MZ * MZ);
        Check(std::fabs(MLength - 1.0f) < 1e-4f, "the packed normal comes back a unit vector",
            std::format("length {:f}", MLength));

        // Splitting is the plain one: three greyscale maps out of three channels
        ImagePixel Red, Green, Blue;
        GameImageProcessors::SplitChannels(Nog, Red, Green, Blue);
        Check(Red.R == 0.25f && Green.G == 0.6f && Blue.B == 0.75f &&
              Red.A == 1.0f && Green.A == 1.0f && Blue.A == 1.0f,
            "splitting a packed texture keeps each channel whole", "");

        ImagePixel Specular, SplitGloss;
        GameImageProcessors::SplitSpecularGloss(Nog, Specular, SplitGloss);
        Check(Specular.R == 0.25f && Specular.G == 0.6f && Specular.B == 0.75f && Specular.A == 1.0f &&
              SplitGloss.R == 0.4f && SplitGloss.B == 0.4f,
            "specular keeps its colour and gloss comes out of alpha", "");
    }

    // Salsa20 against the published eSTREAM vector: Black Ops 2 fast files are encrypted with it.
    {
        // Set 1, vector 0 for a 256 bit key: a single set bit, a zero nonce
        uint8_t Key[32] = {};
        Key[0] = 0x80;
        uint8_t Nonce[8] = {};

        // Encrypting zeroes hands back the raw keystream
        uint8_t Block[64] = {};

        Salsa20Key Cipher;
        Cipher.Key = Key;
        Cipher.KeyLength = 256;
        Cipher.IV = Nonce;

        Encryption::Salsa20Block((int8_t*)Block, sizeof(Block), Cipher);

        const uint8_t Expected[64] = {
            0xE3, 0xBE, 0x8F, 0xDD, 0x8B, 0xEC, 0xA2, 0xE3, 0xEA, 0x8E, 0xF9, 0x47, 0x5B, 0x29, 0xA6, 0xE7,
            0x00, 0x39, 0x51, 0xE1, 0x09, 0x7A, 0x5C, 0x38, 0xD2, 0x3B, 0x7A, 0x5F, 0xAD, 0x9F, 0x68, 0x44,
            0xB2, 0x2C, 0x97, 0x55, 0x9E, 0x27, 0x23, 0xC7, 0xCB, 0xBD, 0x3F, 0xE4, 0xFC, 0x8D, 0x9A, 0x07,
            0x44, 0x65, 0x2A, 0x83, 0xE7, 0x2A, 0x9C, 0x46, 0x18, 0x76, 0xAF, 0x4D, 0x7E, 0xF1, 0xA1, 0x17,
        };

        std::string Got;
        for (size_t i = 0; i < 8; i++)
        {
            Got += std::format("{:02X}", Block[i]);
        }

        Check(std::memcmp(Block, Expected, sizeof(Expected)) == 0,
            "Salsa20 matches the eSTREAM 256 bit vector", Got);

        // A stream cipher is its own inverse, so running it twice restores the input
        uint8_t Message[32];
        for (size_t i = 0; i < sizeof(Message); i++)
        {
            Message[i] = (uint8_t)(i * 7 + 3);
        }

        uint8_t Copy[32];
        std::memcpy(Copy, Message, sizeof(Copy));

        Encryption::Salsa20Block((int8_t*)Copy, sizeof(Copy), Cipher);
        const auto Changed = std::memcmp(Copy, Message, sizeof(Copy)) != 0;
        Encryption::Salsa20Block((int8_t*)Copy, sizeof(Copy), Cipher);

        Check(Changed, "Salsa20 actually alters the buffer", "buffer came back unchanged");
        Check(std::memcmp(Copy, Message, sizeof(Copy)) == 0, "Salsa20 round trips", "did not restore");
    }

    // XMODEL_EXPORT round trip: build a model, write it with the existing writer, read it back
    // with the reader, and compare - which exercises both.
    {
        std::error_code Ec;
        WraithModel Model;

        const char* BoneNames[] = { "tag_origin", "j_mainroot", "j_spine" };
        const int32_t BoneParents[] = { -1, 0, 1 };

        for (int i = 0; i < 3; i++)
        {
            WraithBone Bone;
            Bone.TagName = BoneNames[i];
            Bone.BoneParent = BoneParents[i];
            Bone.GlobalPosition = Vector3((float)i, (float)i * 2.0f, (float)i * 3.0f);
            Bone.BoneScale = Vector3(1.0f, 1.0f, 1.0f);
            Model.Bones.push_back(Bone);
        }

        const auto Path = (std::filesystem::path(FileSystems::GetApplicationPath()) / "selftest_roundtrip.XMODEL_EXPORT").string();

        CodXME::ExportXME(Model, Path);

        XMEModel ReadBack;
        std::string ReadError;
        const auto Ok = CodXMEReader::ReadFile(Path, ReadBack, ReadError);

        Check(Ok, "XMODEL_EXPORT round trip reads", ReadError);

        if (Ok)
        {
            Check(ReadBack.Version == 6, "XMODEL_EXPORT version",
                std::format("{}", ReadBack.Version));

            Check(ReadBack.Bones.size() == 3, "XMODEL_EXPORT bone count",
                std::format("{}", (unsigned long long)ReadBack.Bones.size()));

            if (ReadBack.Bones.size() == 3)
            {
                for (int i = 0; i < 3; i++)
                {
                    Check(ReadBack.Bones[i].Name == BoneNames[i],
                        "XMODEL_EXPORT bone name", ReadBack.Bones[i].Name);

                    Check(ReadBack.Bones[i].Parent == BoneParents[i],
                        "XMODEL_EXPORT bone parent", std::format("{}", ReadBack.Bones[i].Parent));

                    // Positions survive a decimal round trip, so compare with a tolerance
                    const float Expected[3] = { (float)i, (float)i * 2.0f, (float)i * 3.0f };
                    for (int Axis = 0; Axis < 3; Axis++)
                    {
                        Check(std::fabs(ReadBack.Bones[i].Offset[Axis] - Expected[Axis]) < 0.001f,
                            "XMODEL_EXPORT bone offset",
                            std::format("{:f} expected {:f}", ReadBack.Bones[i].Offset[Axis], Expected[Axis]));
                    }
                }
            }
        }

        // Same round trip through the binary form. Only the container is decoded, so this
        // checks the magic and that the block really holds what its header claims, then
        // looks for a bone name in the token stream to be sure it is the model we wrote.
        const auto BinPath = (std::filesystem::path(FileSystems::GetApplicationPath()) / "selftest_roundtrip.XMODEL_BIN").string();

        CodXMB::ExportXMB(Model, BinPath);

        std::vector<uint8_t> Unpacked;
        std::string BinError;
        const auto BinOk = CodXMBReader::ReadFile(BinPath, Unpacked, BinError);

        Check(BinOk, "XMODEL_BIN round trip unpacks", BinError);

        if (BinOk)
        {
            Check(!Unpacked.empty(), "XMODEL_BIN payload is not empty",
                std::format("{}", (unsigned long long)Unpacked.size()));

            const std::string Haystack((const char*)Unpacked.data(), Unpacked.size());
            Check(Haystack.find("j_mainroot") != std::string::npos,
                "XMODEL_BIN carries the bones we wrote", "j_mainroot missing");
        }

        std::filesystem::remove(BinPath, Ec);
        std::filesystem::remove(Path, Ec);
    }

    // The mesh decoder every model reader shares
    Failed += MeshDecode::SelfTest(Failures);

    return Failed;
}


int RunSelfTest(const CliOptions& Options)
{
    std::string Failures;
    const auto Failed = SelfTest(Failures);

    CliReport(Options, std::format(
        "{}result={}\nself_test_failures={}\n",
        Failures.c_str(),
        (Failed == 0) ? "ok" : "error",
        Failed));

    return (Failed == 0) ? 0 : 4;
}


// Classifies a fast file from its header alone, without decompressing anything - the cheap path for
// a sweep that only needs to know what a directory holds (Decompress runs the whole RSA/AES/LZ4/zlib
// pipeline over the entire file).
int RunIdentify(const CliOptions& Options)
{
    const auto Info = FastFile::Identify(Options.Identify);

    const char* FormatName = "unknown";
    switch (Info.Format)
    {
    case FastFileFormat::InfinityWardZLib:    FormatName = "iw-zlib"; break;
    case FastFileFormat::TreyarchChunked:     FormatName = "treyarch-chunked"; break;
    case FastFileFormat::SledgehammerLZ4:     FormatName = "sledgehammer-lz4"; break;
    case FastFileFormat::TreyarchEncrypted:   FormatName = "treyarch-encrypted"; break;
    case FastFileFormat::InfinityWardBlocked: FormatName = "iw-blocked"; break;
    default: break;
    }

    CliReport(Options, std::format(
        "result={}\nfile={}\nmagic={}\nff_version={}\nformat={}\nencrypted={}\n"
        "declared_zone_size={}\ndata_offset={}\nspliced_table_bytes={}\n",
        Info.Format == FastFileFormat::Unknown ? "error" : "ok",
        Options.Identify.c_str(),
        Info.Magic.c_str(),
        Info.Version,
        FormatName,
        Info.Encrypted ? "yes" : "no",
        (unsigned long long)Info.DeclaredZoneSize,
        (unsigned long long)Info.DataOffset,
        (unsigned long long)Info.SplicedTableBytes));

    return Info.Format == FastFileFormat::Unknown ? 5 : 0;
}


// Unpacks a Fast File into its zone. The zone still needs a per-title parser to reach the assets;
// this only handles the container.
int RunDecompressFF(const CliOptions& Options)
{
    const auto Info = FastFile::Identify(Options.DecompressFF);

    std::vector<uint8_t> Zone;
    std::string FailureReason;
    const auto Ok = FastFile::Decompress(Options.DecompressFF, Zone, FailureReason);

    if (Ok && !Options.Out.empty())
    {
        auto Writer = BinaryWriter();
        if (Writer.Create(Options.Out))
        {
            Writer.Write((const int8_t*)Zone.data(), (uint32_t)Zone.size());
        }
        else
        {
            CliReport(Options, std::format(
                "result=error\nreason=could not write {}\n", Options.Out.c_str()));
            return 6;
        }
    }

    // The strings are what identify what a zone holds, and they are reachable without
    // replaying the engine's pointer fixup, so they are the first thing worth having
    std::string StringReport;
    if (Ok && Options.Strings)
    {
        const auto Strings = FastFile::ScanStrings(Zone);
        StringReport = std::format("strings={}\n", (unsigned long long)Strings.size());

        // The Treyarch zones carry a real string table and asset list, so report what
        // parsing them gives alongside what scanning found. Black Ops 2 and Black Ops 3
        // are both Treyarch and share nothing here: one is 32 bit behind its own header
        // and the other 64 bit from the first byte, and even the type ids differ.
        const auto Layout = FastFile::LayoutFor(Info);
        const auto IsT7 = Info.Format == FastFileFormat::TreyarchChunked;
        const auto IsS1 = Info.Format == FastFileFormat::SledgehammerLZ4;

        if (Layout.Known || IsT7 || IsS1)
        {
            ZoneInfo Parsed;
            std::string ParseError;

            // PointerSize is the one thing that separates the 64-bit rebuilds of MW2/MW3
            // from every other Infinity Ward title: same container, same version number,
            // 32-bit zone header, but an asset list of 8-byte padded pointers behind it.
            const auto ParsedOk =
                IsT7 ? FastFile::ParseTreyarchZone(Zone, Parsed, ParseError) :
                IsS1 ? FastFile::ParseSledgehammerZone(Zone, Parsed, ParseError) :
                Layout.PointerSize == 8 ? FastFile::ParseIWZone(Zone, Layout, Parsed, ParseError) :
                       FastFile::Parse32BitZone(Zone, Layout, Parsed, ParseError);

            if (IsS1)
            {
                StringReport += "zone_title=Advanced Warfare\n";
            }
            else if (!IsT7)
            {
                StringReport += std::format("zone_title={}\n", Layout.Title.c_str());
            }

            if (ParsedOk)
            {
                StringReport += std::format(
                    "string_table_count={}\nstring_table_inline={}\nstring_table_offset={}\nstring_table_read={}\n",
                    Parsed.StringCount,
                    Parsed.StringsInline,
                    (unsigned long long)Parsed.StringTableOffset,
                    (unsigned long long)Parsed.Strings.size());

                // How many of the assets carry their header inline says how much of the
                // zone is reachable by reading forwards, and how much needs the blocks
                uint32_t InlineAssets = 0;
                for (const auto& Asset : Parsed.Assets)
                {
                    InlineAssets += Asset.Inline ? 1 : 0;
                }

                // What the zone actually holds, by name - works on every zone whose layout is known.
                // BO3 isn't one of those layouts (64-bit, its own parser) but still has an asset list
                // worth naming, with ids from t7x since OpenAssetTools stops at BO2.
                const auto* const* Names = IsT7 ? ZoneAssetTypes::BlackOps3 : Layout.TypeNames;
                const auto NameCount = IsT7 ? std::size(ZoneAssetTypes::BlackOps3) : Layout.TypeNameCount;

                if (Names != nullptr)
                {
                    std::map<std::string, uint32_t> Counts;

                    for (const auto& Asset : Parsed.Assets)
                    {
                        const auto* TypeName = ZoneAssetTypes::Lookup(Names, NameCount, Asset.Type);

                        Counts[TypeName != nullptr
                            ? TypeName
                            : std::format("type_{}", Asset.Type)]++;
                    }

                    std::string Inventory;
                    for (const auto& Entry : Counts)
                    {
                        if (!Inventory.empty())
                        {
                            Inventory += ",";
                        }
                        Inventory += std::format("{}:{}", Entry.first.c_str(), Entry.second);
                    }

                    StringReport += std::format("asset_types={}\n", Inventory.c_str());
                }

                StringReport += std::format(
                    "asset_count={}\nasset_list_offset={}\nasset_list_read={}\nassets_inline={}\n",
                    Parsed.AssetCount,
                    (unsigned long long)Parsed.AssetListOffset,
                    (unsigned long long)Parsed.Assets.size(),
                    InlineAssets);

                // Then the stream those assets describe, walked by the game's zonekit tables
                ProfileWalk Walk;
                std::string WalkError;

                if (WalkWithProfile(Options.DecompressFF, Zone, Walk, WalkError))
                {
                    // The zone-level assets the walk entered, in list order: the inline entries'
                    // types, and where each one's struct starts
                    std::vector<uint32_t> InlineTypes;
                    for (const auto& Asset : Parsed.Assets)
                    {
                        if (Asset.Inline) InlineTypes.push_back(Asset.Type);
                    }
                    std::vector<const ZoneTables::ZoneResult::Instance*> Entered;
                    for (const auto& I : Walk.R.Instances)
                    {
                        if (I.Parent < 0) Entered.push_back(&I);
                    }

                    StringReport += std::format(
                        "walk_profile={}\nwalk_assets={}\nwalk_complete={}\nwalk_stream_offset={}\nwalk_end_offset={}\nwalk_why={}\n",
                        Walk.Profile,
                        Walk.Walked,
                        Walk.Complete ? "yes" : "no",
                        Entered.empty() ? 0ull : (unsigned long long)Entered.front()->Pos,
                        Entered.empty() ? 0ull : (unsigned long long)Entered.back()->End,
                        Walk.R.Why);

                    // The first inline asset not read whole
                    if (!Walk.Complete)
                    {
                        StringReport += std::format(
                            "walk_stopped_at={}\nwalk_stopped_type={}\n",
                            Walk.Walked, Walk.Walked < InlineTypes.size() ? InlineTypes[Walk.Walked] : 0u);
                    }

                    // Every asset the walk read, by name - "type\tname" per line: the string its
                    // first field points to, a localized string's key its second (the text first).
                    if (!Options.Out.empty() && Walk.Walked > 0)
                    {
                        std::ofstream AssetWriter(Options.Out + ".assets.txt");
                        if (AssetWriter)
                        {
                            const auto* const* Names = IsT7 ? ZoneAssetTypes::BlackOps3 : Layout.TypeNames;
                            const auto NameCount = IsT7 ? std::size(ZoneAssetTypes::BlackOps3) : Layout.TypeNameCount;

                            for (size_t K = 0; K < Walk.Walked && K < InlineTypes.size(); K++)
                            {
                                const uint32_t Type = InlineTypes[K];
                                const auto* TypeName = (Names != nullptr)
                                    ? ZoneAssetTypes::Lookup(Names, NameCount, Type)
                                    : nullptr;
                                const bool Localize = TypeName != nullptr && strcmp(TypeName, "localize_entry") == 0;
                                const std::string Name = Walk.String(Zone, Entered[K]->Pos + (Localize ? Walk.PointerSize : 0));
                                if (Name.empty())
                                {
                                    continue;
                                }

                                // A type whose first field is not its name can point at other
                                // text than a path: every byte of a real name is printable ASCII.
                                const auto IsRealName = std::all_of(
                                    Name.begin(), Name.end(),
                                    [](char C) { return C >= 0x20 && C < 0x7F; });

                                if (!IsRealName)
                                {
                                    continue;
                                }

                                AssetWriter << std::format("{}\t{}",
                                    TypeName != nullptr ? std::string(TypeName) : std::format("type_{}", Type),
                                    Name.c_str()) << '\n';
                            }
                        }
                    }
                }
                else
                {
                    StringReport += std::format("walk_error={}\n", WalkError.c_str());
                }
            }
            else
            {
                StringReport += std::format("string_table_error={}\n", ParseError.c_str());
            }
        }

        if (!Options.Out.empty())
        {
            std::ofstream Writer(Options.Out + ".strings.txt");
            if (Writer)
            {
                for (const auto& Entry : Strings)
                {
                    Writer << std::format("{:08x} {}\n", (unsigned long long)Entry.first, Entry.second.c_str());
                }
            }
        }
    }

    // A failed unpack reports no zone at all. How far it got before giving up is worth
    // knowing, but under its own name: anything reading zone_bytes is asking how much
    // came out, and the answer when this fails is none of it.
    const auto Partial = Ok
        ? std::string()
        : std::format("zone_partial_bytes={}\n", (unsigned long long)Zone.size());

    // The title the module table gives (its install folder or its header deciding between titles
    // that share a version), and the zonekit profile of that row
    const auto* Module = FindGameModule(Info.Magic, Info.Version, Options.DecompressFF);
    const auto ModuleReport = Module == nullptr ? std::string("game=\n")
        : std::format("game={}\ngame_title={}\ngame_profile={}\n", Module->Engine, Module->Title,
                      Module->Profile != nullptr ? Module->Profile : "");

    CliReport(Options, std::format(
        "result={}\nfile={}\nmagic={}\nff_version={}\n{}zone_bytes={}\n{}{}{}{}",
        Ok ? "ok" : "error",
        Options.DecompressFF.c_str(),
        Info.Magic.c_str(),
        Info.Version,
        ModuleReport,
        (unsigned long long)(Ok ? Zone.size() : 0),
        Partial.c_str(),
        StringReport.c_str(),
        Ok ? "" : "reason=",
        Ok ? "" : (FailureReason + "\n").c_str()));

    return Ok ? 0 : 6;
}

// ---- the subcommands (CliCommands.h)

void AddMiscCommands(CliCommandBuilders& B)
{
    B["self-test"] = [](CLI::App& App, CliOptions& Options) {
        App.add_subcommand("self-test", "Run built-in assertions that need no game files or window")
            ->callback([&Options] { Options.SelfTest = true; });
    };
    B["decompress-ff"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("decompress-ff", "Decompress a fast file and report its header/zone info");
        Sub->add_option("file", Options.DecompressFF, "Fast file (.ff) to decompress")->required()->check(CLI::ExistingFile);
        Sub->add_flag("--strings", Options.Strings, "Also dump the zone's string table");
    };
    B["identify"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("identify", "Identify a game/asset by name or hash");
        Sub->add_option("name", Options.Identify, "Name or hash to identify")->required();
    };
}
