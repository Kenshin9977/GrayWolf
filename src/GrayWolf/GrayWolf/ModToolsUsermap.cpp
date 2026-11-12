#include "stdafx.h"

#include "ModToolsUsermap.h"

#include "ExportNaming.h"
#include "OatBridge.h"
#include "Strings.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace ModToolsUsermap
{
namespace
{
    // The assertion boxes a mod tools program put up ("ASSERTION FAILURE...", "SANITY CHECK
    // FAILURE...", a dialog of the linker's or cod2map's, which waits for someone at the screen):
    // each one's program and text,
    // the box answered Abort (the program then ends) so a build never waits on it
    std::vector<std::string> DismissAssertions()
    {
        static const char* const Ours[] = { "gdtdb.exe", "cod2map64.exe", "radiant_modtools.exe", "linker_modtools.exe" };
        std::vector<std::pair<HWND, std::string>> Boxes;
        EnumWindows(
            [](HWND W, LPARAM Found) -> BOOL {
                char Class[64] = {}, Title[256] = {};
                if (!IsWindowVisible(W) || !GetClassNameA(W, Class, sizeof(Class)) || std::string(Class) != "#32770") return TRUE;
                GetWindowTextA(W, Title, sizeof(Title));
                if (std::string(Title).find("FAILURE") == std::string::npos) return TRUE;
                DWORD Pid = 0;
                GetWindowThreadProcessId(W, &Pid);
                char Path[MAX_PATH] = {};
                DWORD Size = MAX_PATH;
                if (HANDLE P = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Pid))
                {
                    QueryFullProcessImageNameA(P, 0, Path, &Size);
                    CloseHandle(P);
                }
                const std::string Exe = ExportNaming::Lower(fs::path(Path).filename().string());
                if (std::find(std::begin(Ours), std::end(Ours), Exe) == std::end(Ours)) return TRUE;
                std::string Text;
                EnumChildWindows(
                    W,
                    [](HWND C, LPARAM Into) -> BOOL {
                        char Cls[32] = {}, Buf[4096] = {};
                        if (GetClassNameA(C, Cls, sizeof(Cls)) && std::string(Cls) == "Static" && GetWindowTextA(C, Buf, sizeof(Buf)) > 0)
                        {
                            std::string& T = *reinterpret_cast<std::string*>(Into);
                            T += (T.empty() ? "" : " ") + std::string(Buf);
                        }
                        return TRUE;
                    },
                    reinterpret_cast<LPARAM>(&Text));
                for (char& Ch : Text)
                    if (Ch == '\r' || Ch == '\n') Ch = ' ';
                reinterpret_cast<std::vector<std::pair<HWND, std::string>>*>(Found)->emplace_back(W, Exe + ": " + Text);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&Boxes));
        std::vector<std::string> Out;
        for (const auto& [W, Text] : Boxes)
        {
            PostMessageA(W, WM_COMMAND, IDABORT, 0);
            Out.push_back(Text);
        }
        return Out;
    }

    std::string ReadAll(const fs::path& P)
    {
        std::ifstream In(P, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
    }

    std::string ReplaceAll(std::string S, const std::string& From, const std::string& To)
    {
        for (size_t At = S.find(From); At != std::string::npos; At = S.find(From, At + To.size())) S.replace(At, From.size(), To);
        return S;
    }

    using ExportNaming::Num;

    // The origin of Text's first entity of Class ("" when it has none)
    bool OriginOf(const std::string& Text, const std::string& Class, std::array<double, 3>& Out)
    {
        const size_t At = Text.find("\"classname\" \"" + Class + "\"");
        if (At == std::string::npos) return false;
        const size_t Begin = Text.rfind("\n{", At), End = Text.find("\n}", At);
        const std::string Block = Text.substr(Begin == std::string::npos ? 0 : Begin, End == std::string::npos ? std::string::npos : End - Begin);
        std::smatch M;
        static const std::regex Origin("\"origin\" \"([-0-9.e]+) ([-0-9.e]+) ([-0-9.e]+)\"");
        if (!std::regex_search(Block, M, Origin)) return false;
        for (int A = 0; A < 3; A++) Out[A] = std::stod(M[A + 1].str());
        return true;
    }

    // Where the source puts players: its info_player_start (MW3's Spec Ops maps: info_player_start_so
    // and _soPlayer2), then a multiplayer map's spawn points (mp_<mode>_spawn..., mp_spawn_point); Most at most
    std::vector<std::array<double, 3>> SpawnsOf(const std::string& Text, size_t Most)
    {
        std::vector<std::array<double, 3>> Out;
        // a plain scan: the text is tens of megabytes. Each entity whose class starts with Start
        // and holds Part, its origin.
        const auto Scan = [&](const std::string& Start, const char* Part) {
            const std::string Class = "\"classname\" \"" + Start, Origin = "\"origin\" \"";
            for (size_t At = Text.find(Class); At != std::string::npos && Out.size() < Most; At = Text.find(Class, At + 1))
            {
                const size_t NameEnd = Text.find('"', At + Class.size());
                const std::string Name = Text.substr(At + Class.size(), NameEnd - At - Class.size());
                if (Name.find(Part) == std::string::npos) continue;
                const size_t End = Text.find("\n}", At), O = Text.find(Origin, At);
                if (O == std::string::npos || O > End) continue;
                std::istringstream Values(Text.substr(O + Origin.size(), 96));
                double V[3];
                if (Values >> V[0] >> V[1] >> V[2]) Out.push_back({ V[0], V[1], V[2] });
            }
        };
        Scan("info_player_start", "");
        Scan("mp_", "spawn");   // mp_dm_spawn, mp_tdm_spawn_allies_start; BO4 and Cold War: mp_spawn_point
        return Out;
    }

    // The template's .map: its header, then its entities, worldspawn's keys only (its room left
    // out), every other entity's origin and brush points moved by By, but where players appear
    // (info_player_start, the initial_spawn_points and player_respawn_point structs) on the
    // source's own Spawns in turn (the template's, around its start, land in the source's walls),
    // and the player volume stretched over Area (low corner, high corner): the zombies scripts
    // kill a player outside it (BO1's mp_firingrange: a spawn point past the template's box, a
    // game over on appearing), the sun volume over SunArea (the level's sun, its global probe and
    // its baked light: the template's left most of firing range unlit, its floors black), baking
    // with Light's sky (its runtime one the template's), auto exposure on within Light's range,
    // Light's reflection probes added; then Prefab as a misc_prefab
    struct Lighting
    {
        std::string BakeSsi;                          // the sun volume's ssi1, the template's its runtime override; empty: as it is
        double Ev = std::numeric_limits<double>::quiet_NaN();   // the runtime SSI's exposure: auto exposure within a stop of it
        std::vector<std::array<double, 9>> Probes;    // box reflection probes: origin, extents below it, above it
    };
    std::string Level(const std::string& Template, const std::array<double, 3>& By, const std::vector<std::array<double, 3>>& Spawns,
                      const std::array<double, 6>& Area, const std::array<double, 6>& SunArea, const Lighting& Light, const std::string& Prefab)
    {
        static const std::regex Ssi1("^\"ssi1\" \"([^\"]*)\"$");
        size_t NextSpawn = 0;
        std::vector<std::string> Lines;
        {
            std::istringstream In(Template);
            for (std::string L; std::getline(In, L);) Lines.push_back(L.empty() || L.back() != '\r' ? L : L.substr(0, L.size() - 1));
        }
        static const std::regex Origin("^\"origin\" \"([-0-9.e]+) ([-0-9.e]+) ([-0-9.e]+)\"$");
        static const std::regex Point("\\(\\s*([-0-9.e]+)\\s+([-0-9.e]+)\\s+([-0-9.e]+)\\s*\\)");
        const auto Moved = [&](const std::smatch& M) {
            std::string S;
            for (int A = 0; A < 3; A++) S += (A ? " " : "") + Num(std::stod(M[A + 1].str()) + By[A]);
            return S;
        };
        std::string Out;
        size_t Entity = 0;
        for (size_t I = 0; I < Lines.size();)
        {
            if (Strings::Trim(Lines[I]) != "{")
            {
                // the header (the layers); the template's "// entity N" lines are written again
                if (Entity == 0 && Lines[I].rfind("// entity", 0) != 0 && !Strings::Trim(Lines[I]).empty()) Out += Lines[I] + "\n";
                I++;
                continue;
            }
            // an entity: its lines up to its closing brace
            size_t End = I, Depth = 0;
            for (; End < Lines.size(); End++)
            {
                const std::string T = Strings::Trim(Lines[End]);
                if (T == "{") Depth++;
                else if (T == "}" && --Depth == 0) break;
            }
            bool World = false, PlayerSpot = false;
            const std::array<double, 6>* Fit = nullptr;   // the box the entity's brush is stretched over
            for (size_t K = I; K <= End && K < Lines.size(); K++)
            {
                World = World || Lines[K] == "\"classname\" \"worldspawn\"";
                PlayerSpot = PlayerSpot || Lines[K] == "\"classname\" \"info_player_start\"" || Lines[K] == "\"targetname\" \"initial_spawn_points\"" ||
                             Lines[K] == "\"targetname\" \"player_respawn_point\"";
                if (Lines[K] == "\"script_noteworthy\" \"player_volume\"") Fit = &Area;
                if (Lines[K] == "\"classname\" \"volume_sun\"") Fit = &SunArea;
            }
            // the volume's box in the template, each of its points then mapped onto Fit
            std::array<double, 6> Box = { 1e300, 1e300, 1e300, -1e300, -1e300, -1e300 };
            for (size_t K = I; Fit && K <= End && K < Lines.size(); K++)
                if (Strings::Trim(Lines[K]).rfind("(", 0) == 0)
                    for (std::sregex_iterator It(Lines[K].begin(), Lines[K].end(), Point), None; It != None; ++It)
                        for (int A = 0; A < 3; A++)
                        {
                            const double V = std::stod((*It)[A + 1].str());
                            Box[A] = std::min(Box[A], V), Box[A + 3] = std::max(Box[A + 3], V);
                        }
            const auto Placed = [&](const std::smatch& M) {
                if (!Fit) return Moved(M);
                std::string S;
                for (int A = 0; A < 3; A++)
                {
                    const double Span = Box[A + 3] - Box[A], V = std::stod(M[A + 1].str());
                    const double T = Span > 0 ? (V - Box[A]) / Span : 0.0;
                    S += (A ? " " : "") + Num((*Fit)[A] + T * ((*Fit)[A + 3] - (*Fit)[A]));
                }
                return S;
            };
            Out += "// entity " + std::to_string(Entity++) + "\n";
            Depth = 0;
            for (size_t K = I; K <= End && K < Lines.size(); K++)
            {
                const std::string& L = Lines[K];
                const std::string T = Strings::Trim(L);
                const size_t Was = Depth;
                if (T == "{") Depth++;
                else if (T == "}") Depth--;
                if (World)
                {
                    // its keys and braces only: a brush (its "// brush" line, its block) left out
                    if (Was >= 2 || Depth >= 2 || (Was == 1 && L.rfind("// brush", 0) == 0)) continue;
                    Out += L + "\n";
                    // auto exposure (Lighting_Parameters: set only in the worldspawn) within a stop
                    // of the SSI's exposure: Treyarch's levels narrow it so (The Giant 3 to 3.5,
                    // Combine 8 to 13.5); the SSI's own range (default_day: 1 to 16) let a dim view
                    // brighten 14 stops, a glow over everything
                    if (L == "\"classname\" \"worldspawn\"")
                    {
                        Out += "\"evauto\" \"1\"\n";
                        if (std::isfinite(Light.Ev)) Out += "\"evmin\" \"" + Num(Light.Ev - 1.0) + "\"\n\"evmax\" \"" + Num(Light.Ev + 1.0) + "\"\n";
                    }
                    continue;
                }
                std::smatch M;
                if (Fit == &SunArea && !Light.BakeSsi.empty() && std::regex_match(L, M, Ssi1))
                {
                    // SunSky_Light_SetUp: ssi1 bakes the GI, ssi1_runtime_override shows in game
                    Out += "\"ssi1\" \"" + Light.BakeSsi + "\"\n\"ssi1_runtime_override\" \"" + M[1].str() + "\"\n";
                }
                else if (std::regex_match(L, M, Origin) && PlayerSpot && !Spawns.empty())
                {
                    const auto& S = Spawns[NextSpawn++ % Spawns.size()];
                    Out += "\"origin\" \"" + Num(S[0]) + " " + Num(S[1]) + " " + Num(S[2]) + "\"\n";
                }
                else if (std::regex_match(L, M, Origin)) Out += "\"origin\" \"" + Moved(M) + "\"\n";
                else if (T.rfind("(", 0) == 0)
                {
                    std::string Line;
                    auto From = L.cbegin();
                    for (std::sregex_iterator It(L.begin(), L.end(), Point), None; It != None; ++It)
                    {
                        Line.append(From, (*It)[0].first);
                        Line += "( " + Placed(*It) + " )";
                        From = (*It)[0].second;
                    }
                    Line.append(From, L.cend());
                    Out += Line + "\n";
                }
                else
                    Out += L + "\n";
            }
            I = End + 1;
        }
        // the reflection probes, as the template's (a box, its extents each side of its origin)
        for (const auto& P : Light.Probes)
        {
            const std::string Below = Num(P[3]) + " " + Num(P[4]) + " " + Num(P[5]), Above = Num(P[6]) + " " + Num(P[7]) + " " + Num(P[8]);
            Out += "// entity " + std::to_string(Entity++) + "\n{\n\"classname\" \"reflection_probe\"\n\"origin\" \"" + Num(P[0]) + " " + Num(P[1]) + " " +
                   Num(P[2]) + "\"\n\"box\" \"1\"\n\"size_min\" \"" + Below + "\"\n\"size_max\" \"" + Above +
                   "\"\n\"blend_mins\" \"3 3 3\"\n\"blend_maxs\" \"3 3 3\"\n\"in_play_space\" \"1\"\n\"high_detail\" \"1\"\n\"client_server\" \"ClientSide\"\n}\n";
        }
        Out += "// entity " + std::to_string(Entity) + "\n{\n\"classname\" \"misc_prefab\"\n\"model\" \"" + Prefab + "\"\n\"origin\" \"0 0 0\"\n}\n";
        return Out;
    }

    size_t ErrorLines(const std::string& Log)
    {
        size_t N = 0;
        std::istringstream In(Log);
        for (std::string L; std::getline(In, L);) N += L.find("ERROR") != std::string::npos;   // "^1ERROR: ...", the linker's colour code first
        return N;
    }
}

std::string NameOf(const std::string& Map)
{
    return "zm_" + ExportNaming::SafeFileName(ExportNaming::Lower(Map));
}

namespace
{
    // usermaps/<name>/zone/workshop.json when it has none: the game (and boiii) lists and loads a
    // usermap by it, which the launcher writes only when publishing to the Workshop. Its
    // PublisherID must be a number: one of the name's, ten digits, steady from run to run.
    void WriteWorkshop(const fs::path& Usermap, const std::string& Name, const std::string& Map)
    {
        const fs::path File = Usermap / "zone" / "workshop.json";
        std::error_code Ec;
        if (fs::exists(File, Ec)) return;
        fs::create_directories(File.parent_path(), Ec);
        const uint64_t Id = 1000000000ull + ExportNaming::Fnv1a(Name) % 9000000000ull;
        std::ofstream(File, std::ios::binary) << "{\n\t\"PublisherID\": \"" << Id << "\",\n\t\"Title\": \"" << Map
                                              << " (GrayWolf)\",\n\t\"Description\": \"" << Map
                                              << ", exported by GrayWolf\",\n\t\"FolderName\": \"" << Name << "\",\n\t\"Type\": \"map\"\n}\n";
    }

    // One entry of a GDT's text, its name line to its closing "\t}", renamed Name, each of Keys'
    // values set (a key it has not added); empty when the GDT has no From
    std::string CopiedEntry(const std::string& Gdt, const std::string& From, const std::string& Name,
                            const std::vector<std::pair<std::string, std::string>>& Keys)
    {
        const std::string Head = "\t\"" + From + "\" (";
        const size_t At = Gdt.find(Head), End = At == std::string::npos ? At : Gdt.find("\n\t}", At);
        if (End == std::string::npos) return std::string();
        std::string E = "\t\"" + Name + "\" (" + Gdt.substr(At + Head.size(), End + 3 - At - Head.size());
        const std::string Eol = E.find("\r\n") != std::string::npos ? "\r\n" : "\n";
        for (const auto& [Key, Value] : Keys)
        {
            const std::string K = "\t\t\"" + Key + "\" \"";
            if (const size_t P = E.find(K); P != std::string::npos)
            {
                const size_t V = P + K.size(), Close = E.find('"', V);
                E.replace(V, Close - V, Value);
            }
            else
                E.insert(E.find("\t{") + 2 + Eol.size(), K + Value + "\"" + Eol);
        }
        return E;
    }

    // An SSI's exposure ("ev") in a GDT's text, NaN when it has no such entry or key
    double SsiExposure(const std::string& Gdt, const std::string& Ssi)
    {
        const size_t At = Gdt.find("\t\"" + Ssi + "\" ("), End = At == std::string::npos ? At : Gdt.find("\n\t}", At);
        if (End == std::string::npos) return std::numeric_limits<double>::quiet_NaN();
        static const std::regex Ev("\"ev\" \"([-0-9.]+)\"");
        const std::string Entry = Gdt.substr(At, End - At);
        std::smatch M;
        return std::regex_search(Entry, M, Ev) ? std::stod(M[1].str()) : std::numeric_limits<double>::quiet_NaN();
    }

    // The template's day sky (default_day: skybox_default_day, mtl_skybox_default_day) again for
    // the light bake, tinted grey: lit by the sky as it is, the bounced light turns every surface
    // blue (SunSky_Light_SetUp's "bake override"). Written to source_data/graywolf_<Name>_sky.gdt
    // when it is not there; its SSI's name, empty when the mod tools lack the stock entries.
    std::string BakeSky(const fs::path& Root, const std::string& Name)
    {
        // what makes the sky's mean grey at its luminance: its upper half's mean linear colour,
        // weighed by solid angle and cosine (skybox_default_day_ll.exr), 0.2534 0.3402 0.5408
        constexpr const char* Grey = "1.3268 0.9883 0.6218 1";
        const std::string Ssi = Name + "_bake_ssi", Model = Name + "_bake_skybox", Material = Name + "_bake_sky";
        const fs::path Out = Root / "source_data" / ("graywolf_" + Name + "_sky.gdt");
        std::error_code Ec;
        if (fs::exists(Out, Ec)) return Ssi;
        const std::string Props = ReadAll(Root / "model_export" / "t7_techart_props.gdt"), Ssis = ReadAll(Root / "source_data" / "ssi.gdt");
        const std::string M = CopiedEntry(Props, "mtl_skybox_default_day", Material, { { "colorTint", Grey } });
        const std::string X = CopiedEntry(Props, "skybox_default_day", Model, { { "skinOverride", "mtl_skybox_default " + Material + "\\r\\n" } });
        const std::string S = CopiedEntry(Ssis, "default_day", Ssi, { { "skyboxmodel", Model } });
        if (M.empty() || X.empty() || S.empty()) return std::string();
        std::ofstream(Out, std::ios::binary) << "{\r\n" << M << "\r\n" << X << "\r\n" << S << "\r\n}\r\n";
        return Ssi;
    }
}

Made Create(const fs::path& Root, const std::string& Map, const std::string& Prefab)
{
    Made R;
    R.Name = NameOf(Map);
    const fs::path Template = Root / "rex" / "templates" / "ZM Mod Level";
    const fs::path Usermap = Root / "usermaps" / R.Name, Level_ = Root / "map_source" / "zm" / (R.Name + ".map");
    std::error_code Ec;
    // made before: a user may have edited it, kept; only its workshop.json added when it has none.
    // A half left (its folder or its level deleted): the missing half made again
    const bool HasUsermap = fs::exists(Usermap, Ec), HasLevel = fs::exists(Level_, Ec);
    if (HasUsermap) WriteWorkshop(Usermap, R.Name, Map);
    if (HasUsermap && HasLevel) return R;
    if (!fs::exists(Template / "map_source" / "zm" / "template.map", Ec))
    {
        R.Error = "no ZM Mod Level template in " + (Root / "rex" / "templates").string();
        return R;
    }
    if (!HasUsermap)
    {
        // usermaps/<name>: the template's files, "template" renamed in their paths and their text
        const fs::path Files = Template / "usermaps" / "template";
        for (const auto& E : fs::recursive_directory_iterator(Files, Ec))
        {
            if (!E.is_regular_file()) continue;
            const fs::path Out = Usermap / fs::path(ReplaceAll(fs::relative(E.path(), Files).generic_string(), "template", R.Name));
            fs::create_directories(Out.parent_path(), Ec);
            const std::string Ext = ExportNaming::Lower(E.path().extension().string());
            if (Ext == ".gsc" || Ext == ".csc" || Ext == ".szc" || Ext == ".zone")
                std::ofstream(Out, std::ios::binary) << ReplaceAll(ReadAll(E.path()), "template", R.Name);
            else
                fs::copy_file(E.path(), Out, fs::copy_options::overwrite_existing, Ec);
        }
        WriteWorkshop(Usermap, R.Name, Map);
        R.Created = true;
    }
    if (HasLevel) return R;
    std::array<double, 3> To{}, From{};
    const std::string Source = ReadAll(Root / "map_source" / fs::path(Prefab));
    const std::vector<std::array<double, 3>> Spawns = SpawnsOf(Source, 16);
    // the player volume: around every spawn point the source has, a room's width aside, a storey
    // below and a few above
    std::array<double, 6> Area = { 1e300, 1e300, 1e300, -1e300, -1e300, -1e300 };
    for (const auto& S : SpawnsOf(Source, std::numeric_limits<size_t>::max()))
        for (int A = 0; A < 3; A++) Area[A] = std::min(Area[A], S[A]), Area[A + 3] = std::max(Area[A + 3], S[A]);
    for (int A = 0; A < 3; A++)
    {
        Area[A] -= A < 2 ? 1024.0 : 256.0;
        Area[A + 3] += 1024.0;
    }
    // the sun's: past the spawn points as far as a map's scenery goes (firing range's reaches
    // 3800 units past its last spawn point), a few storeys up
    std::array<double, 6> SunArea = Area;
    for (int A = 0; A < 3; A++)
    {
        SunArea[A] -= A < 2 ? 4096.0 : 768.0;
        SunArea[A + 3] += A < 2 ? 4096.0 : 3072.0;
    }
    if (Spawns.empty())
    {
        R.Error = "map_source/" + Prefab + " has no info_player_start nor spawn point to put the player at";
        return R;
    }
    To = Spawns.front();
    const std::string Text = ReadAll(Template / "map_source" / "zm" / "template.map");
    if (!OriginOf(Text, "info_player_start", From))
    {
        R.Error = "the ZM Mod Level template's .map has no info_player_start";
        return R;
    }
    const std::array<double, 3> By = { To[0] - From[0], To[1] - From[1], To[2] - From[2] };
    fs::create_directories(Level_.parent_path(), Ec);
    std::ofstream Out(Level_, std::ios::binary);
    // reflection probes in a grid over the play area, a box each of its cell, its point a player's
    // eye above the lowest spawn point: else everything reflects the template's one probe, 72
    // units a side, or the sky (Treyarch's maps place one a room, The Giant 176)
    Lighting Light;
    Light.BakeSsi = BakeSky(Root, R.Name);
    Light.Ev = SsiExposure(ReadAll(Root / "source_data" / "ssi.gdt"), "default_day");
    constexpr double Cell = 1024.0;
    const double Eye = Area[2] + 256.0 + 72.0;   // Area starts 256 below the lowest spawn point
    for (double X = Area[0] + Cell / 2; X < Area[3]; X += Cell)
        for (double Y = Area[1] + Cell / 2; Y < Area[4]; Y += Cell)
            Light.Probes.push_back({ X, Y, Eye, Cell / 2, Cell / 2, Eye - Area[2], Cell / 2, Cell / 2, Area[5] - Eye });
    Out << Level(Text, By, Spawns, Area, SunArea, Light, fs::path(Prefab).generic_string());
    if (!Out.good()) R.Error = "cannot write " + Level_.string();
    R.Created = R.Error.empty();
    return R;
}

std::vector<Step> Build(const fs::path& Root, const std::string& Name, const std::function<void(const std::string&)>& Line,
                        const std::function<bool()>& Poll)
{
    const std::string R = Root.string(), Slash = R + "\\";
    // the variables the launcher's modtools_launcher.bat sets
    const std::vector<std::pair<std::string, std::string>> Env = {
        { "TA_GAME_PATH", Slash }, { "TA_TOOLS_PATH", Slash }, { "TA_LOCAL_ASSET_CACHE", (Root / "share" / "assetconvert").string() + "\\" } };
    const std::string Level = (Root / "map_source" / "zm" / (Name + ".map")).string();
    std::error_code Ec;
    fs::create_directories(Root / "share" / "raw" / "maps" / "zm", Ec);
    struct Tool
    {
        const char* Label;
        fs::path Exe;
        std::vector<std::string> Args;
    };
    // as the launcher runs them (its output names each command)
    const std::vector<Tool> Tools = {
        { "gdtdb", Root / "gdtdb" / "gdtdb.exe", { "/update" } },
        { "compile", Root / "bin" / "cod2map64.exe",
          { "-platform", "pc", "-navmesh", "-navvolume", "-loadFrom", Level, (Root / "share" / "raw" / "maps" / "zm" / (Name + ".d3dbsp")).string() } },
        { "light", Root / "bin" / "radiant_modtools.exe", { "-ledSilent", "+medium", "+localprobes", "+forceclean", "+recompute", Level } },
        { "link", Root / "bin" / "linker_modtools.exe", { "-language", "english", "-modsource", Name } } };
    std::vector<Step> Steps;
    // the port's writer found a limit the tools refuse (<map>.limits.txt beside its .map): no
    // tool run, minutes before the one that would fail
    if (const fs::path Limits = Root / "map_source" / "graywolf" / ((Name.rfind("zm_", 0) == 0 ? Name.substr(3) : Name) + ".limits.txt");
        fs::exists(Limits, Ec))
    {
        Step S;
        S.Label = "limits";
        S.Log = Limits.string();
        S.Exit = 1;
        std::ifstream In(Limits);
        for (std::string L; std::getline(In, L);)
            if (!L.empty()) S.Errors++, Line("limits: " + L);
        Steps.push_back(S);
        return Steps;
    }
    for (const Tool& T : Tools)
    {
        Step S;
        S.Label = T.Label;
        S.Log = (Root / "usermaps" / Name / ("graywolf_" + S.Label + ".log")).string();
        Line(S.Label + ": " + T.Exe.filename().string() + "...");
        const auto Start = std::chrono::steady_clock::now();
        const fs::path Zone = Root / "usermaps" / Name / "zone" / (Name + ".ff");
        const auto Before = fs::exists(Zone, Ec) ? fs::last_write_time(Zone, Ec) : fs::file_time_type::min();
        std::string Output;
        // while it runs, an assertion box it puts up read and dismissed
        const auto Watch = [&] {
            for (const std::string& A : DismissAssertions())
            {
                Line(S.Label + ": assertion dismissed: " + A);
                S.Assertion += (S.Assertion.empty() ? "" : " | ") + A;
            }
            return Poll();
        };
        S.Exit = RunTool(T.Exe.string(), T.Args, T.Exe.parent_path().string(), S.Log, Env, Watch, Output);
        if (!S.Assertion.empty())
        {
            std::ofstream(S.Log, std::ios::app) << "\nGrayWolf: assertion dismissed: " << S.Assertion << "\n";
            if (S.Exit == 0) S.Exit = 1;
        }
        // the linker's exit code is not 0 when it links (19105000 for a level that linked): its
        // zone written anew is what tells
        if (S.Label == "link" && S.Exit != 0xFFFFFFF0 && S.Exit != 0xFFFFFFF2)
        {
            const bool Written = fs::exists(Zone, Ec) && fs::last_write_time(Zone, Ec) > Before;
            if (S.Exit != 0) Line("link: exit " + std::to_string((long)S.Exit) + (Written ? ", the zone written: linked" : ", no zone written"));
            S.Exit = Written ? 0 : (S.Exit ? S.Exit : 1);
        }
        S.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
        S.Errors = ErrorLines(Output);
        Line(S.Label + ": exit " + std::to_string((long)S.Exit) + " in " + std::to_string((long)S.Seconds) + " s, " + std::to_string(S.Errors) +
             " ERROR lines (" + S.Log + ")");
        Steps.push_back(S);
        if (S.Exit != 0) break;
    }
    return Steps;
}
}
