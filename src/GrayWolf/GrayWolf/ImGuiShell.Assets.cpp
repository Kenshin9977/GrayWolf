#include "stdafx.h"

// The Assets tab: the folders shown, the search box, the type tabs, the numeric filters and presets,
// the rows (a table or AssetGrid) and their picks.
#include "ImGuiShell.Internal.h"
#include "AppLog.h"           // timed steps
#include "ExportNaming.h"      // Lower
#include "UiAsync.h"           // the folder dialogs
#include "SettingsManager.h"   // the list or the grid
#include "FxPreviewCells.h"    // an effect in its grid cell

#include "imgui.h"
#include <nlohmann/json.hpp>   // filter presets

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace ShellUi
{
    bool NameMatches(const std::string& Query, const std::string& Name)
    {
        // Everything-style: whitespace-separated terms ANDed, '!' negates, case-insensitive.
        using ExportNaming::Lower;
        const std::string LName = Lower(Name);
        size_t i = 0;
        bool any = false;
        while (i < Query.size())
        {
            while (i < Query.size() && isspace((unsigned char)Query[i])) i++;
            size_t j = i; while (j < Query.size() && !isspace((unsigned char)Query[j])) j++;
            if (j > i)
            {
                any = true;
                std::string Term = Query.substr(i, j - i);
                bool Neg = false; if (Term[0]=='!'){ Neg=true; Term.erase(Term.begin()); }
                if (!Term.empty())
                {
                    const bool Has = LName.find(Lower(Term)) != std::string::npos;
                    if (Neg ? Has : !Has) return false;
                }
            }
            i = j;
        }
        (void)any; return true;
    }

    // Human-readable file size, Windows-Explorer style: 1024-based steps labelled KB/MB/GB.
    std::string FormatSize(long long Bytes)
    {
        if (Bytes < 0) Bytes = 0;
        const char* Units[] = { "B", "KB", "MB", "GB", "TB" };
        double V = (double)Bytes; int U = 0;
        while (V >= 1024.0 && U < 4) { V /= 1024.0; ++U; }
        char Buf[32];
        if (U == 0)      snprintf(Buf, sizeof Buf, "%lld B", Bytes);
        else if (V >= 100) snprintf(Buf, sizeof Buf, "%.0f %s", V, Units[U]);
        else             snprintf(Buf, sizeof Buf, "%.1f %s", V, Units[U]);
        return Buf;
    }

    namespace fs = std::filesystem;
    static const fs::path PresetsDir = "filter_presets";

    void RefreshPresets(AssetsTab& A)
    {
        A.Presets.clear();
        std::error_code Ec;
        if (fs::is_directory(PresetsDir, Ec))
            for (auto& E : fs::directory_iterator(PresetsDir, Ec))
                if (E.path().extension() == ".json") A.Presets.push_back(E.path().stem().string());
        std::sort(A.Presets.begin(), A.Presets.end());
    }

    namespace
    {
        void SavePreset(AssetsTab& A, const std::string& Name)
        {
            std::error_code Ec; fs::create_directories(PresetsDir, Ec);
            nlohmann::json J;
            J["name"] = std::string(A.Filter);
            J["minBones"] = A.fMinBones; J["maxBones"] = A.fMaxBones;
            J["minMeshes"] = A.fMinMeshes; J["maxMeshes"] = A.fMaxMeshes;
            J["minMats"] = A.fMinMats; J["maxMats"] = A.fMaxMats;
            J["minKB"] = A.fMinKB; J["maxKB"] = A.fMaxKB;
            std::ofstream O((PresetsDir / (Name + ".json")).string(), std::ios::binary | std::ios::trunc);
            if (O) O << J.dump(2);
            RefreshPresets(A);
        }

        void LoadPreset(AssetsTab& A, const std::string& Name)
        {
            std::ifstream I((PresetsDir / (Name + ".json")).string(), std::ios::binary);
            if (!I) return;
            nlohmann::json J; try { I >> J; } catch (...) { return; }
            const std::string Nm = J.value("name", std::string());
            std::snprintf(A.Filter, sizeof(A.Filter), "%s", Nm.c_str());
            A.fMinBones = J.value("minBones", 0); A.fMaxBones = J.value("maxBones", 0);
            A.fMinMeshes = J.value("minMeshes", 0); A.fMaxMeshes = J.value("maxMeshes", 0);
            A.fMinMats = J.value("minMats", 0); A.fMaxMats = J.value("maxMats", 0);
            A.fMinKB = J.value("minKB", 0); A.fMaxKB = J.value("maxKB", 0);
        }

        // Ctrl+C on the Assets table and the row context menu's "Copy name"/"Copy path" both land
        // here: every MultiSel pick, one per line, or just Selected when nothing is multi-picked.
        void CopySelection(const ShellState& S, bool Paths)
        {
            const auto& Models = S.Catalog.Models;
            std::vector<int> Idxs;
            if (!S.MultiSel.empty()) Idxs.assign(S.MultiSel.begin(), S.MultiSel.end());
            else if (S.Selected >= 0) Idxs.push_back(S.Selected);
            std::sort(Idxs.begin(), Idxs.end());
            std::string Text;
            for (int i : Idxs)
                if (i >= 0 && i < (int)Models.size()) Text += (Paths ? Models[i].Path : Models[i].Name) + "\n";
            if (!Text.empty()) { Text.pop_back(); ImGui::SetClipboardText(Text.c_str()); }
        }

        // The rows: Models filtered and sorted (AssetsTab::View).
        void BuildView(ShellState& S)
        {
            AssetsTab& A = S.Assets;
            const auto& Models = S.Catalog.Models;
            const std::string Key = std::to_string(A.TypeFilter) + "|" + std::to_string(A.HideModelTextures) + "|" + std::to_string(A.SortCol) + "|" +
                std::to_string(A.SortAsc) + "|" + std::to_string(A.fMinBones) + "," + std::to_string(A.fMaxBones) + "," + std::to_string(A.fMinMeshes) + "," +
                std::to_string(A.fMaxMeshes) + "," + std::to_string(A.fMinMats) + "," + std::to_string(A.fMaxMats) + "," + std::to_string(A.fMinKB) + "," +
                std::to_string(A.fMaxKB) + "|" + std::to_string(A.GroupByGame) + "|" + A.GameFilter + "|" + A.Filter;
            // Models in another order (a merge, a replace) invalidates View's indices: built at once;
            // header fields filled in only move rows: a few times a second
            const bool HeadersMatter = (A.SortCol >= 3 && A.SortCol <= 6) || A.fMinBones || A.fMaxBones || A.fMinMeshes || A.fMaxMeshes || A.fMinMats || A.fMaxMats;
            if (Key == A.ViewKey && A.ViewVersion == S.Catalog.ModelsVersion &&
                (!HeadersMatter || A.ViewHeaders == S.Catalog.HeaderVersion || ImGui::GetTime() - A.ViewBuiltAt < 0.3)) return;
            AppLog::Timer Build("build the asset view");
            A.ViewKey = Key;
            A.ViewVersion = S.Catalog.ModelsVersion;
            A.ViewHeaders = S.Catalog.HeaderVersion;
            A.ViewBuiltAt = ImGui::GetTime();
            A.View.clear(); A.View.reserve(Models.size());
            for (int i = 0; i < (int)Models.size(); i++)
            {
                const Item& M = Models[i];
                if (A.TypeFilter == 1 && M.Kind != 0) continue;
                if (A.TypeFilter == 2 && M.Kind != 1) continue;
                if (A.TypeFilter == 3 && M.Kind != 2) continue;
                if (A.TypeFilter == 4 && M.Kind != 3) continue;
                if (A.TypeFilter == 5 && M.Kind != 4) continue;
                if (A.TypeFilter == 6 && M.Kind != 5) continue;
                if (A.HideModelTextures && M.Kind == 1 && M.ModelTexture) continue;
                if (!A.GameFilter.empty() && M.Game != A.GameFilter) continue;
                if (A.Filter[0] && !NameMatches(A.Filter, M.Name)) continue;
                if ((A.fMinBones  > 0 && (int)M.Bones  < A.fMinBones)  || (A.fMaxBones  > 0 && (int)M.Bones  > A.fMaxBones))  continue;
                if ((A.fMinMeshes > 0 && (int)M.Meshes < A.fMinMeshes) || (A.fMaxMeshes > 0 && (int)M.Meshes > A.fMaxMeshes)) continue;
                if ((A.fMinMats   > 0 && (int)M.Mats   < A.fMinMats)   || (A.fMaxMats   > 0 && (int)M.Mats   > A.fMaxMats))   continue;
                { const long long KB = M.SizeBytes / 1024;
                  if ((A.fMinKB > 0 && KB < A.fMinKB) || (A.fMaxKB > 0 && KB > A.fMaxKB)) continue; }
                A.View.push_back(i);
            }
            // Models is kept sorted by name: that order needs no sort
            if (A.SortCol == 0) { if (!A.SortAsc) std::reverse(A.View.begin(), A.View.end()); }
            else std::sort(A.View.begin(), A.View.end(), [&](int a, int b) {
                const Item& L = Models[a]; const Item& R = Models[b]; bool lt;
                switch (A.SortCol) {
                    case 1: lt = L.Game   < R.Game;   break;
                    case 2: lt = L.Kind   < R.Kind;   break;
                    case 3: lt = L.Bones  < R.Bones;  break;
                    case 4: lt = L.Meshes < R.Meshes; break;
                    case 5: lt = L.Mats   < R.Mats;   break;
                    case 6: lt = (long long)L.ImgW * L.ImgH < (long long)R.ImgW * R.ImgH; break;
                    case 7: lt = L.Format < R.Format; break;
                    case 8: lt = L.SizeBytes < R.SizeBytes; break;
                    default: lt = L.Name  < R.Name;   break;
                }
                return A.SortAsc ? lt : !lt;
            });
            // grouped: each game's rows together, in the order above
            if (A.GroupByGame)
                std::stable_sort(A.View.begin(), A.View.end(), [&](int a, int b) { return Models[a].Game < Models[b].Game; });
        }

        // The games listed (each once, sorted), again only when Models changed.
        void RefreshGames(ShellState& S)
        {
            AssetsTab& A = S.Assets;
            if (A.GamesVersion == S.Catalog.ModelsVersion) return;
            A.GamesVersion = S.Catalog.ModelsVersion;
            A.Games.clear();
            for (const Item& M : S.Catalog.Models)
                if (!M.Game.empty() && std::find(A.Games.begin(), A.Games.end(), M.Game) == A.Games.end()) A.Games.push_back(M.Game);
            std::sort(A.Games.begin(), A.Games.end());
        }

        // Which game's assets are shown, and the rows grouped by game: only when more than one game is listed.
        void DrawGameFilter(ShellState& S)
        {
            AssetsTab& A = S.Assets;
            RefreshGames(S);
            if (A.Games.size() < 2 && A.GameFilter.empty()) return;
            const float Em = ImGui::GetFontSize();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Game");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(Em * 9.0f);
            if (ImGui::BeginCombo("##game", A.GameFilter.empty() ? "All games" : A.GameFilter.c_str()))
            {
                if (ImGui::Selectable("All games", A.GameFilter.empty())) A.GameFilter.clear();
                for (const auto& G : A.Games)
                    if (ImGui::Selectable(G.c_str(), A.GameFilter == G)) A.GameFilter = G;
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::Checkbox("Group by game", &A.GroupByGame);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Each game's assets together (the same asset from two games side by side otherwise)");
        }

        // Any folder of exported assets, in place of the current list or added to it, and what is listed.
        void DrawFolderBar(ShellState& S)
        {
            const auto& Shown = S.Catalog.ShownFolders;
            // the dialog runs on its own thread: its answer is collected on the frame it closes
            const std::string StartIn = Shown.empty() ? std::string() : Shown.back();
            if (ImGui::Button("Open folder...")) UiAsync::PickFolder("assets.open", "Select a folder of exported assets", StartIn);
            if (std::string Dir; UiAsync::Picked("assets.open", Dir)) OpenFolder(S, Dir, true);
            ImGui::SameLine();
            if (ImGui::Button("Add folder...")) UiAsync::PickFolder("assets.add", "Select a folder of exported assets to add", StartIn);
            if (std::string Dir; UiAsync::Picked("assets.add", Dir)) OpenFolder(S, Dir, false);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keep the assets listed and add this folder's");
            ImGui::SameLine();
            if (S.Catalog.Walking())
            {
                const std::string Dir = S.Catalog.WalkedDir();
                ImGui::TextDisabled("scanning %s\xE2\x80\xA6  %zu files", Dir.c_str(), S.Catalog.Seen());
            }
            else if (Shown.empty()) ImGui::TextDisabled("no folder open");
            else if (Shown.size() == 1) ImGui::TextDisabled("%s", Shown[0].c_str());
            else ImGui::TextDisabled("%s (+%d more)", Shown.back().c_str(), (int)Shown.size() - 1);
            if (Shown.size() > 1 && ImGui::IsItemHovered())
            {
                std::string All;
                for (const auto& F : Shown) All += F + "\n";
                ImGui::SetTooltip("%s", All.c_str());
            }
        }

        // Search box: full-row hint field, an inline clear (x) shown only when there's text, then the help (?).
        void DrawSearchBox(AssetsTab& A, bool& OpenHelp)
        {
            const float BtnW = ImGui::GetFrameHeight();
            const float Sp   = ImGui::GetStyle().ItemSpacing.x;
            const bool  HasText = A.Filter[0] != '\0';
            float Reserve = BtnW + Sp;                      // "?" button
            if (HasText) Reserve += BtnW + Sp;              // "x" clear button
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - Reserve);
            ImGui::InputTextWithHint("##filter", "Search assets by name\xE2\x80\xA6  (Ctrl+F, Esc clears)",
                                     A.Filter, sizeof(A.Filter), ImGuiInputTextFlags_EscapeClearsAll);
            if (HasText)
            {
                ImGui::SameLine();
                if (ImGui::Button("\xC3\x97", ImVec2(BtnW, 0))) A.Filter[0] = '\0';
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear filter");
            }
            ImGui::SameLine();
            if (ImGui::Button("?", ImVec2(BtnW, 0))) OpenHelp = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Filter syntax help");
        }

        // Type selector — segmented "pills" (cleaner than default tabs). Active = accent fill,
        // inactive = flat + muted, all equal width.
        void DrawTypeTabs(AssetsTab& A)
        {
            const float Em = ImGui::GetFontSize();
            // Indices 4/5 (Anims/Maps) come after 0-3 to keep TypeFilter's meanings (1=Models, 2=Images, 3=Audio).
            static const char* Segs[] = { "All", "Models", "Images", "Audio", "Anims", "Maps", "FX" };
            const ImVec4 AccOn(0.24f, 0.40f, 0.60f, 1.0f), AccOnHov(0.30f, 0.48f, 0.70f, 1.0f);
            const ImVec4 Flat(1,1,1,0.04f), FlatHov(1,1,1,0.10f);
            const ImVec4 Muted = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            const float SegW = Em * 4.4f;
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Em * 0.9f);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, ImGui::GetStyle().ItemSpacing.y));
            for (int i = 0; i < 7; i++)
            {
                const bool On = (A.TypeFilter == i);
                if (i > 0) ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button,        On ? AccOn    : Flat);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, On ? AccOnHov : FlatHov);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  On ? AccOnHov : FlatHov);
                if (!On) ImGui::PushStyleColor(ImGuiCol_Text, Muted);
                if (ImGui::Button(Segs[i], ImVec2(SegW, 0))) A.TypeFilter = i;
                ImGui::PopStyleColor(On ? 3 : 4);
            }
            ImGui::PopStyleVar(2);
        }

        // Numeric filters (min-max ranges; 0 = no limit). Each value is a segmented stepper:
        // [ - ][ number ][ + ] — minus left, the (editable) value in the middle, plus right,
        // joined in one bordered box.
        void DrawNumericFilters(AssetsTab& A)
        {
            if (!ImGui::CollapsingHeader("Numeric filters", ImGuiTreeNodeFlags_DefaultOpen)) return;
            const float Em = ImGui::GetFontSize();
            ImGui::TextDisabled("0 = no limit");
            const ImVec4 Cell   = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
            const ImVec4 CellHi(Cell.x + 0.06f, Cell.y + 0.06f, Cell.z + 0.07f, 1.0f);
            auto Stepper = [&](const char* Id, int* V, int Step) {
                const float H = ImGui::GetFrameHeight();
                ImGui::PushID(Id);
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Button,        Cell);   // -/+ match the value cell -> one unit
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, CellHi);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  CellHi);
                ImGui::PushButtonRepeat(true);
                ImGui::BeginGroup();
                if (ImGui::Button("\xE2\x88\x92", ImVec2(H, H))) { *V -= Step; if (*V < 0) *V = 0; } // U+2212 minus
                ImGui::SameLine();
                ImGui::SetNextItemWidth(Em * 3.2f);
                if (ImGui::InputInt("##n", V, 0, 0) && *V < 0) *V = 0;
                ImGui::SameLine();
                if (ImGui::Button("+", ImVec2(H, H))) *V += Step;
                ImGui::EndGroup();
                ImGui::PopButtonRepeat();
                ImGui::PopStyleColor(3);
                ImGui::PopStyleVar(3);
                ImGui::PopID();
            };
            auto RangeRow = [&](const char* Label, int* Lo, int* Hi, int Step) {
                ImGui::PushID(Label);
                ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(Label);
                ImGui::SameLine(Em * 4.4f);
                Stepper("lo", Lo, Step);
                ImGui::SameLine(0, 7); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("\xE2\x80\x93"); ImGui::SameLine(0, 7);
                Stepper("hi", Hi, Step);
                ImGui::PopID();
            };
            RangeRow("Bones",  &A.fMinBones,  &A.fMaxBones,  1);
            RangeRow("Meshes", &A.fMinMeshes, &A.fMaxMeshes, 1);
            RangeRow("Mats",   &A.fMinMats,   &A.fMaxMats,   1);
            RangeRow("Size KB",&A.fMinKB,     &A.fMaxKB,     64);
            ImGui::Spacing();
            if (ImGui::SmallButton("Reset numeric")) { A.fMinBones=A.fMaxBones=A.fMinMeshes=A.fMaxMeshes=A.fMinMats=A.fMaxMats=A.fMinKB=A.fMaxKB=0; }
        }

        void DrawPresets(AssetsTab& A)
        {
            if (!ImGui::CollapsingHeader("Filter presets")) return;
            static char PresetName[64] = "";
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70.0f);
            ImGui::InputTextWithHint("##presetname", "preset name", PresetName, sizeof(PresetName));
            ImGui::SameLine();
            if (ImGui::Button("Save", ImVec2(64, 0)) && PresetName[0]) SavePreset(A, PresetName);
            if (A.Presets.empty()) ImGui::TextDisabled("no saved presets");
            for (const auto& P : A.Presets)
            {
                ImGui::PushID(P.c_str());
                if (ImGui::SmallButton("Load")) LoadPreset(A, P);
                ImGui::SameLine(); ImGui::TextUnformatted(P.c_str());
                ImGui::PopID();
            }
        }

        // List | Grid, the grid's cell size, the rows shown and the picks.
        void DrawViewBar(ShellState& S)
        {
            AssetsTab& A = S.Assets;
            const float Sp = ImGui::GetStyle().ItemSpacing.x;
            {
                // List | Grid, and the grid's cell size
                const auto Seg = [&](const char* Label, bool On) {
                    ImGui::PushStyleColor(ImGuiCol_Button, On ? ImVec4(0.24f, 0.40f, 0.60f, 1.0f) : ImVec4(1, 1, 1, 0.04f));
                    const bool Hit = ImGui::SmallButton(Label);
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0, 2);
                    return Hit && !On;
                };
                if (Seg("List", !A.GridView) | Seg("Grid", A.GridView))
                {
                    A.GridView = !A.GridView;
                    SettingsManager::SetSetting("assets_view", A.GridView ? "grid" : "list");
                }
                if (A.GridView && !A.Grid)
                {
                    A.Grid = std::make_unique<AssetGrid>(S.Dev.Get(), S.Ctx.Get());
                    A.Grid->SetRenderer(AssetKind::Fx, MakeFxCells());
                    A.Grid->SetAnimRig(A.GridRig);
                    A.Grid->SetAnimRigResolver([Rigs = S.Rigs](const std::string& Anim, std::vector<std::string>& Paths) {
                        AnimRigs::Pick Pick;
                        if (!Rigs->PickFor(Anim, Pick)) return false;
                        Paths = Pick.Paths;
                        return true;
                    });
                    if (S.ShotMode && S.ShotMode->Cell > 0) A.Grid->SetCellSize(S.ShotMode->Cell);
                    if (S.ShotMode) A.Grid->SetTurning(!S.ShotMode->Still);
                }
                ImGui::SameLine(0, Sp);
                if (A.GridView) { A.Grid->DrawSizeSlider(); ImGui::SameLine(); }
            }
            ImGui::TextDisabled(A.GridView ? "%d / %d assets" : "%d / %d assets  (click a header to sort)", (int)A.View.size(), (int)S.Catalog.Models.size());
            if (!S.MultiSel.empty())
            {
                ImGui::SameLine();
                ImGui::TextDisabled("\xC2\xB7  %d picked (Ctrl+click to build a set)", (int)S.MultiSel.size());
                ImGui::SameLine();
                if (ImGui::SmallButton("Merge")) LoadMerged(S, S.MultiSel);
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear picks")) S.MultiSel.clear();
            }
            ImGui::Separator();
        }

        void HandleKeys(ShellState& S)
        {
            const AssetsTab& A = S.Assets;
            const auto& View = A.View;
            // Keyboard row navigation: Up/Down walks the filtered/sorted View, not raw Models,
            // so it always lands on what's actually visible. Skipped while a widget (e.g. the
            // filter box) has active input capture, so arrow keys still move a text cursor there.
            if (!View.empty() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive())
            {
                int CurPos = -1;
                for (int k = 0; k < (int)View.size(); k++) if (View[k] == S.Selected) { CurPos = k; break; }
                bool Moved = false;
                const int Step = A.GridView && A.Grid ? A.Grid->Columns() : 1;   // the grid: Up/Down a row, Left/Right a cell
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) { CurPos = (CurPos < 0) ? 0 : std::min(CurPos + Step, (int)View.size() - 1); Moved = true; }
                else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) { CurPos = (CurPos < 0) ? 0 : std::max(CurPos - Step, 0); Moved = true; }
                else if (A.GridView && ImGui::IsKeyPressed(ImGuiKey_RightArrow)) { CurPos = (CurPos < 0) ? 0 : std::min(CurPos + 1, (int)View.size() - 1); Moved = true; }
                else if (A.GridView && ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) { CurPos = (CurPos < 0) ? 0 : std::max(CurPos - 1, 0); Moved = true; }
                if (Moved) { LoadSelection(S, View[CurPos]); S.ScrollToSelected = true; }
            }
            // Ctrl+C copies the selection's name(s) - same focus/active-item gate as the nav above, but
            // not gated on View: a multi-pick can still include rows the current type tab is hiding.
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive()
                && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C))
                CopySelection(S, false);
        }

        // A row's or a cell's click: Ctrl+click builds a pick set for "Merge" without disturbing the
        // live preview; a plain click previews normally and starts a fresh pick set.
        void ClickAsset(ShellState& S, int idx)
        {
            if (ImGui::GetIO().KeyCtrl) { if (S.MultiSel.count(idx)) S.MultiSel.erase(idx); else S.MultiSel.insert(idx); }
            else { S.MultiSel.clear(); LoadSelection(S, idx); }
        }

        // Right-click outside the current selection selects first (Explorer-style), so "Copy name"
        // on the menu always acts on what it looks like it acts on.
        void AssetContextMenu(ShellState& S, int idx)
        {
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && S.Selected != idx && !S.MultiSel.count(idx))
            {
                S.MultiSel.clear();
                LoadSelection(S, idx);
            }
            if (ImGui::BeginPopupContextItem("row_context"))
            {
                if (ImGui::MenuItem("Copy name")) CopySelection(S, false);
                if (ImGui::MenuItem("Copy path")) CopySelection(S, true);
                ImGui::EndPopup();
            }
        }

        void DrawGrid(ShellState& S)
        {
            AssetsTab& A = S.Assets;
            const auto& View = A.View;
            const auto& Models = S.Catalog.Models;
            int ScrollTo = -1;
            if (S.ScrollToSelected)
            {
                for (int k = 0; k < (int)View.size(); k++) if (View[k] == S.Selected) { ScrollTo = k; break; }
                S.ScrollToSelected = false;
            }
            A.Grid->SetLighting(S.Viewport.Lighting);   // the cells look like the viewport
            if (S.Shot) A.Grid->SetAutoScroll(S.Shot->Measuring() ? S.ShotMode->Scroll : 0.0f);
            A.Grid->Draw((int)View.size(),
                [&](int Row) {
                    const Item& M = Models[View[Row]];
                    AssetGridItem It;
                    It.Path = &M.Path; It.Name = &M.Name; It.Format = &M.Format; It.Kind = M.Kind;
                    if (A.Games.size() > 1) It.Game = &M.Game;   // one game listed: no need to say which
                    It.SizeBytes = M.SizeBytes; It.Written = M.Written; It.Id = View[Row];
                    return It;
                },
                [&](int Row) { return View[Row] == S.Selected || S.MultiSel.count(View[Row]) != 0; },
                [&](int Row) {
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) ClickAsset(S, View[Row]);
                    AssetContextMenu(S, View[Row]);
                },
                ScrollTo);
        }

        void DrawTable(ShellState& S)
        {
            AssetsTab& A = S.Assets;
            const auto& Models = S.Catalog.Models;
            const float Em = ImGui::GetFontSize();
            // ScrollX + SizingFixedFit: every column keeps its own width, so widening one grows the
            // table (horizontal scrollbar) instead of stealing from its neighbours. No Stretch column
            // here (Stretch + ScrollX don't mix). Name column is pinned while scrolling.
            const ImGuiTableFlags TFlags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg
                | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit
                | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable;
            if (!ImGui::BeginTable("assets", 9, TFlags, ImVec2(0.0f, 0.0f), 0.0f)) return;
            ImGui::TableSetupColumn("Name",   ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, Em * 12.0f);
            ImGui::TableSetupColumn("Game",   ImGuiTableColumnFlags_WidthFixed, Em * 4.5f);
            ImGui::TableSetupColumn("Type",   ImGuiTableColumnFlags_WidthFixed, Em * 3.4f);
            ImGui::TableSetupColumn("Bones",  ImGuiTableColumnFlags_WidthFixed, Em * 3.6f);
            ImGui::TableSetupColumn("Meshes", ImGuiTableColumnFlags_WidthFixed, Em * 3.6f);
            ImGui::TableSetupColumn("Mats",   ImGuiTableColumnFlags_WidthFixed, Em * 3.6f);
            ImGui::TableSetupColumn("Dim",    ImGuiTableColumnFlags_WidthFixed, Em * 6.4f);
            ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, Em * 4.5f);
            ImGui::TableSetupColumn("Size",   ImGuiTableColumnFlags_WidthFixed, Em * 4.5f);
            ImGui::TableSetupScrollFreeze(1, 1);
            // Per-kind columns don't make sense outside their tab: Bones applies to models AND anims
            // (bone count an anim drives), Meshes to models AND maps (mesh count either way), Mats
            // only to models, Dim shows image WxH or map verts/tris. On the mixed "All" tab, leave
            // everything visible.
            // Game: only with assets of more than one game listed.
            ImGui::TableSetColumnEnabled(1, A.Games.size() > 1);                                          // Game
            ImGui::TableSetColumnEnabled(3, A.TypeFilter == 0 || A.TypeFilter == 1 || A.TypeFilter == 4); // Bones
            ImGui::TableSetColumnEnabled(4, A.TypeFilter == 0 || A.TypeFilter == 1 || A.TypeFilter == 5); // Meshes
            ImGui::TableSetColumnEnabled(5, A.TypeFilter == 0 || A.TypeFilter == 1); // Mats
            ImGui::TableSetColumnEnabled(6, A.TypeFilter == 0 || A.TypeFilter == 2 || A.TypeFilter == 5); // Dim
            ImGui::TableHeadersRow();

            if (ImGuiTableSortSpecs* Ss = ImGui::TableGetSortSpecs())
                if (Ss->SpecsCount > 0) { A.SortCol = Ss->Specs[0].ColumnIndex; A.SortAsc = (Ss->Specs[0].SortDirection == ImGuiSortDirection_Ascending); }
            BuildView(S);
            const auto& View = A.View;

            // only the rows on screen are submitted (and the selected one, to scroll it into view)
            ImGuiListClipper Clipper;
            Clipper.Begin((int)View.size());
            if (S.ScrollToSelected)
                for (int k = 0; k < (int)View.size(); k++)
                    if (View[k] == S.Selected) { Clipper.IncludeItemByIndex(k); break; }
            while (Clipper.Step())
            for (int Row = Clipper.DisplayStart; Row < Clipper.DisplayEnd; Row++)
            {
                const int idx = View[Row];
                const Item& M = Models[idx];
                const char* Ty = (M.Kind == 0) ? "model" : (M.Kind == 1) ? "image" : (M.Kind == 2) ? "audio" : (M.Kind == 3) ? "anim" : (M.Kind == 4) ? "map" : "fx";
                ImGui::PushID(idx); // names can repeat (e.g. shared texture stems) - disambiguate the row
                ImGui::TableNextRow();
                if (S.ScrollToSelected && S.Selected == idx) { ImGui::SetScrollHereY(); S.ScrollToSelected = false; }
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(M.Name.c_str(), S.Selected == idx || S.MultiSel.count(idx) != 0, ImGuiSelectableFlags_SpanAllColumns))
                    ClickAsset(S, idx);
                AssetContextMenu(S, idx);
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(M.Game.c_str());
                ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("%s", Ty);
                ImGui::TableSetColumnIndex(3); if (M.Kind == 0 || M.Kind == 3) ImGui::Text("%u", M.Bones);
                ImGui::TableSetColumnIndex(4); if (M.Kind == 0 || M.Kind == 4) ImGui::Text("%u", M.Meshes);
                ImGui::TableSetColumnIndex(5); if (M.Kind == 0) ImGui::Text("%u", M.Mats);
                ImGui::TableSetColumnIndex(6);
                if (M.Kind == 1 && M.ImgW > 0) ImGui::Text("%dx%d", M.ImgW, M.ImgH);
                else if (M.Kind == 4) ImGui::Text("%uv / %ut", M.Verts, M.Tris);
                ImGui::TableSetColumnIndex(7); ImGui::TextDisabled("%s", M.Format.c_str());
                ImGui::TableSetColumnIndex(8);
                { std::string Sz = FormatSize(M.SizeBytes);
                  float Tw = ImGui::CalcTextSize(Sz.c_str()).x;
                  float Av = ImGui::GetContentRegionAvail().x;
                  if (Av > Tw) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (Av - Tw)); // right-align sizes
                  ImGui::TextUnformatted(Sz.c_str()); }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    void DrawAssetsPanel(ShellState& S, bool& OpenHelp)
    {
        AssetsTab& A = S.Assets;
        // opened on a folder (ui <dir>): its assets tab comes first, not Load Game's
        static int FramesShown = 0;
        if (FramesShown++ == 1 && !S.ModelsDir.empty()) ImGui::SetNextWindowFocus();
        ImGui::Begin("Assets");
        DrawFolderBar(S);
        DrawSearchBox(A, OpenHelp);
        DrawTypeTabs(A);
        DrawGameFilter(S);
        if (A.TypeFilter == 0 || A.TypeFilter == 2)
            ImGui::Checkbox("Hide model textures (the _images of 3D models)", &A.HideModelTextures);
        DrawNumericFilters(A);
        DrawPresets(A);

        // Built with the last known sort before keyboard nav reads it, so Up/Down walk what is on
        // screen; again once BeginTable has this frame's sort (only if a header was just clicked).
        BuildView(S);
        DrawViewBar(S);
        HandleKeys(S);
        if (A.GridView && A.Grid) DrawGrid(S);
        else DrawTable(S);
        ImGui::End();
    }
}
