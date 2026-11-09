#include "stdafx.h"

#include "ModToolsUsermap.h"

#include "ExportNaming.h"
#include "OatBridge.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
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

    std::string Num(double V)
    {
        char B[64];
        std::snprintf(B, sizeof(B), "%.4f", V);
        std::string S = B;
        while (!S.empty() && S.back() == '0') S.pop_back();
        if (!S.empty() && S.back() == '.') S.pop_back();
        return S == "-0" ? "0" : S;
    }

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
    // and _soPlayer2), then a multiplayer map's spawn points (mp_<mode>_spawn..., mp_spawn_point); 16 at most
    std::vector<std::array<double, 3>> SpawnsOf(const std::string& Text)
    {
        std::vector<std::array<double, 3>> Out;
        // a plain scan: the text is tens of megabytes. Each entity whose class starts with Start
        // and holds Part, its origin.
        const auto Scan = [&](const std::string& Start, const char* Part) {
            const std::string Class = "\"classname\" \"" + Start, Origin = "\"origin\" \"";
            for (size_t At = Text.find(Class); At != std::string::npos && Out.size() < 16; At = Text.find(Class, At + 1))
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

    std::string Trimmed(const std::string& S)
    {
        const size_t B = S.find_first_not_of(" \t\r"), E = S.find_last_not_of(" \t\r");
        return B == std::string::npos ? std::string() : S.substr(B, E - B + 1);
    }

    // The template's .map: its header, then its entities, worldspawn's keys only (its room left
    // out), every other entity's origin and brush points moved by By, but where players appear
    // (info_player_start, the initial_spawn_points and player_respawn_point structs) on the
    // source's own Spawns in turn (the template's, around its start, land in the source's walls);
    // then Prefab as a misc_prefab
    std::string Level(const std::string& Template, const std::array<double, 3>& By, const std::vector<std::array<double, 3>>& Spawns,
                      const std::string& Prefab)
    {
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
            if (Trimmed(Lines[I]) != "{")
            {
                // the header (the layers); the template's "// entity N" lines are written again
                if (Entity == 0 && Lines[I].rfind("// entity", 0) != 0 && !Trimmed(Lines[I]).empty()) Out += Lines[I] + "\n";
                I++;
                continue;
            }
            // an entity: its lines up to its closing brace
            size_t End = I, Depth = 0;
            for (; End < Lines.size(); End++)
            {
                const std::string T = Trimmed(Lines[End]);
                if (T == "{") Depth++;
                else if (T == "}" && --Depth == 0) break;
            }
            bool World = false, PlayerSpot = false;
            for (size_t K = I; K <= End && K < Lines.size(); K++)
            {
                World = World || Lines[K] == "\"classname\" \"worldspawn\"";
                PlayerSpot = PlayerSpot || Lines[K] == "\"classname\" \"info_player_start\"" || Lines[K] == "\"targetname\" \"initial_spawn_points\"" ||
                             Lines[K] == "\"targetname\" \"player_respawn_point\"";
            }
            Out += "// entity " + std::to_string(Entity++) + "\n";
            Depth = 0;
            for (size_t K = I; K <= End && K < Lines.size(); K++)
            {
                const std::string& L = Lines[K];
                const std::string T = Trimmed(L);
                const size_t Was = Depth;
                if (T == "{") Depth++;
                else if (T == "}") Depth--;
                if (World)
                {
                    // its keys and braces only: a brush (its "// brush" line, its block) left out
                    if (Was >= 2 || Depth >= 2 || (Was == 1 && L.rfind("// brush", 0) == 0)) continue;
                    Out += L + "\n";
                    continue;
                }
                std::smatch M;
                if (std::regex_match(L, M, Origin) && PlayerSpot && !Spawns.empty())
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
                        Line += "( " + Moved(*It) + " )";
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
}

Made Create(const fs::path& Root, const std::string& Map, const std::string& Prefab)
{
    Made R;
    R.Name = NameOf(Map);
    const fs::path Template = Root / "rex" / "templates" / "ZM Mod Level";
    const fs::path Usermap = Root / "usermaps" / R.Name, Level_ = Root / "map_source" / "zm" / (R.Name + ".map");
    std::error_code Ec;
    if (fs::exists(Usermap, Ec) || fs::exists(Level_, Ec))
    {
        // made before: a user may have edited it; only its workshop.json added when it has none
        if (fs::exists(Usermap, Ec)) WriteWorkshop(Usermap, R.Name, Map);
        return R;
    }
    if (!fs::exists(Template / "map_source" / "zm" / "template.map", Ec))
    {
        R.Error = "no ZM Mod Level template in " + (Root / "rex" / "templates").string();
        return R;
    }
    std::array<double, 3> To{}, From{};
    const std::vector<std::array<double, 3>> Spawns = SpawnsOf(ReadAll(Root / "map_source" / fs::path(Prefab)));
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
    const std::array<double, 3> By = { To[0] - From[0], To[1] - From[1], To[2] - From[2] };
    fs::create_directories(Level_.parent_path(), Ec);
    std::ofstream Out(Level_, std::ios::binary);
    Out << Level(Text, By, Spawns, fs::path(Prefab).generic_string());
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
