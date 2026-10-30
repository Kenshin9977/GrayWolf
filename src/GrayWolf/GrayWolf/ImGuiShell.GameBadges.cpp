#include "stdafx.h"

// GameBadges.h: a game's title and icon from the id of the folder its assets were exported under.
#include "GameBadges.h"
#include "DiskGame.h"        // the readers' titles
#include "GameInstalls.h"    // the installs found here, their exe
#include "LauncherIcons.h"   // an exe's icon as a texture

#include "imgui.h"

#include <atomic>
#include <cctype>
#include <map>
#include <mutex>
#include <thread>

namespace GameBadges
{
    namespace
    {
        // the exe of an install of each reader id, filled once by a worker
        struct Exes
        {
            std::mutex Lock;
            std::map<std::string, std::string> ById, ByPath;
            std::atomic<bool> Started{ false }, Done{ false };
        };

        Exes& Found()
        {
            static Exes E;
            if (!E.Started.exchange(true))
                std::thread([] {
                    for (const auto& I : GameInstalls::Detect())
                    {
                        const std::string Exe = GameInstalls::GameExe(I.Path);
                        if (Exe.empty()) continue;
                        std::lock_guard<std::mutex> Guard(Found().Lock);
                        Found().ByPath.emplace(I.Path, Exe);
                        if (!I.DiskId.empty()) Found().ById.emplace(I.DiskId, Exe);   // the first install of a game stands for it
                    }
                    Found().Done = true;
                }).detach();
            return E;
        }

        const DiskGame::IDiskGame* Reader(const std::string& Id) { return Id.empty() ? nullptr : DiskGame::Find(Id); }

        // the icon in a Side x Side square at Min, else Letters on a raised square
        void Square(ImDrawList* DL, ID3D11ShaderResourceView* Icon, const std::string& Letters, const ImVec2& Min, float Side)
        {
            const ImVec2 Max(Min.x + Side, Min.y + Side);
            if (Icon != nullptr)
            {
                DL->AddImage((ImTextureID)(intptr_t)Icon, Min, Max);
                return;
            }
            DL->AddRectFilled(Min, Max, ImGui::GetColorU32(ImGuiCol_FrameBgActive), Side * 0.2f);
            ImFont* F = ImGui::GetFont();
            const float Size = Side * 0.55f;
            const ImVec2 T = F->CalcTextSizeA(Size, FLT_MAX, 0.0f, Letters.c_str());
            DL->AddText(F, Size, ImVec2(Min.x + (Side - T.x) * 0.5f, Min.y + (Side - T.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), Letters.c_str());
        }
    }

    std::string Title(const std::string& Id)
    {
        const DiskGame::IDiskGame* G = Reader(Id);
        return G ? G->Title() : Id;
    }

    std::string ShortTitle(const std::string& Id)
    {
        std::string T = Title(Id);
        static const std::string Prefix = "Call of Duty: ";
        if (T.rfind(Prefix, 0) == 0) T = T.substr(Prefix.size());
        return T;
    }

    ID3D11ShaderResourceView* Icon(ID3D11Device* Dev, const std::string& Id)
    {
        Exes& E = Found();
        if (!E.Done) return nullptr;
        // the folder's id may be a reader's alias: the reader's own id finds its install
        const DiskGame::IDiskGame* G = Reader(Id);
        const std::string Key = G ? G->Id() : Id;
        std::string Exe;
        {
            std::lock_guard<std::mutex> Guard(E.Lock);
            if (const auto It = E.ById.find(Key); It != E.ById.end()) Exe = It->second;
        }
        return Exe.empty() ? nullptr : LauncherIcons::ForFile(Dev, Exe);
    }

    void Draw(ImDrawList* DL, ID3D11Device* Dev, const std::string& Id, const ImVec2& Min, float Side)
    {
        // no icon: the id's first two letters
        std::string Letters;
        for (const char C : Id)
            if (std::isalnum((unsigned char)C) && Letters.size() < 2) Letters += (char)std::toupper((unsigned char)C);
        Square(DL, Icon(Dev, Id), Letters, Min, Side);
    }

    void DrawInstall(ImDrawList* DL, ID3D11Device* Dev, const std::string& Folder, const std::string& Title, const ImVec2& Min, float Side)
    {
        Exes& E = Found();
        std::string Exe;
        if (E.Done)
        {
            std::lock_guard<std::mutex> Guard(E.Lock);
            if (const auto It = E.ByPath.find(Folder); It != E.ByPath.end()) Exe = It->second;
        }
        // no icon: the first letters of the title's first two words ("Black Ops III" -> "BO")
        std::string Letters;
        for (size_t k = 0; k < Title.size() && Letters.size() < 2; k++)
            if ((k == 0 || Title[k - 1] == ' ') && std::isalnum((unsigned char)Title[k])) Letters += (char)std::toupper((unsigned char)Title[k]);
        Square(DL, Exe.empty() ? nullptr : LauncherIcons::ForFile(Dev, Exe), Letters, Min, Side);
    }
}
