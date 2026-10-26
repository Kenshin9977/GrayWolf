#include "stdafx.h"

#include "AssetGrid.h"
#include "CellRenderers.h"
#include "GridServices.h"
#include "SettingsManager.h"   // the cell size

#include "imgui.h"
#include "imgui_internal.h"    // RenderTextEllipsis

#include <algorithm>
#include <cmath>
#include <string>

const char* AssetKindName(int Kind)
{
    static const char* Names[] = { "model", "image", "audio", "anim", "map", "fx" };
    return Kind >= 0 && Kind < AssetKind::Count ? Names[Kind] : "asset";
}

void DrawAssetKindIcon(ImDrawList* DL, const ImVec2& Min, const ImVec2& Max, int Kind)
{
    DL->AddRectFilled(Min, Max, IM_COL32(30, 33, 40, 255));
    const float S = std::min(Max.x - Min.x, Max.y - Min.y);
    const ImVec2 C((Min.x + Max.x) * 0.5f, (Min.y + Max.y) * 0.5f);
    const float R = S * 0.24f, W = std::max(1.5f, S * 0.022f);
    const ImU32 Col = IM_COL32(150, 165, 190, 200), Dim = IM_COL32(150, 165, 190, 90);
    const auto P = [&](float X, float Y) { return ImVec2(C.x + X * R, C.y + Y * R); };
    switch (Kind)
    {
    case AssetKind::Model:   // a cube
    {
        const ImVec2 Top = P(0, -1), Rt = P(0.87f, -0.5f), Rb = P(0.87f, 0.5f), Bot = P(0, 1), Lb = P(-0.87f, 0.5f), Lt = P(-0.87f, -0.5f), Mid = P(0, 0);
        const ImVec2 Hex[] = { Top, Rt, Rb, Bot, Lb, Lt };
        DL->AddPolyline(Hex, 6, Col, ImDrawFlags_Closed, W);
        DL->AddLine(Lt, Mid, Col, W); DL->AddLine(Rt, Mid, Col, W); DL->AddLine(Mid, Bot, Col, W);
        break;
    }
    case AssetKind::Image:   // a picture: a hill and a sun
    {
        DL->AddRect(P(-1.1f, -0.85f), P(1.1f, 0.85f), Col, R * 0.1f, 0, W);
        const ImVec2 Hill[] = { P(-0.9f, 0.65f), P(-0.25f, -0.15f), P(0.2f, 0.35f), P(0.5f, 0.05f), P(0.9f, 0.65f) };
        DL->AddPolyline(Hill, 5, Col, ImDrawFlags_None, W);
        DL->AddCircleFilled(P(0.5f, -0.4f), R * 0.16f, Col);
        break;
    }
    case AssetKind::Audio:   // a speaker and its waves
    {
        const ImVec2 Body[] = { P(-1.0f, -0.3f), P(-0.55f, -0.3f), P(-0.05f, -0.75f), P(-0.05f, 0.75f), P(-0.55f, 0.3f), P(-1.0f, 0.3f) };
        DL->AddPolyline(Body, 6, Col, ImDrawFlags_Closed, W);
        DL->PathArcTo(P(-0.05f, 0), R * 0.45f, -0.8f, 0.8f, 12); DL->PathStroke(Col, 0, W);
        DL->PathArcTo(P(-0.05f, 0), R * 0.85f, -0.8f, 0.8f, 16); DL->PathStroke(Dim, 0, W);
        break;
    }
    case AssetKind::Anim:    // a play mark in a ring
    {
        DL->AddCircle(C, R * 1.05f, Col, 32, W);
        DL->AddTriangleFilled(P(-0.3f, -0.5f), P(0.55f, 0), P(-0.3f, 0.5f), Col);
        break;
    }
    case AssetKind::Map:     // a folded map
    {
        const ImVec2 Fold[] = { P(-1.0f, -0.6f), P(-0.35f, -0.85f), P(0.35f, -0.6f), P(1.0f, -0.85f), P(1.0f, 0.6f), P(0.35f, 0.85f), P(-0.35f, 0.6f), P(-1.0f, 0.85f) };
        DL->AddPolyline(Fold, 8, Col, ImDrawFlags_Closed, W);
        DL->AddLine(Fold[1], Fold[6], Dim, W); DL->AddLine(Fold[2], Fold[5], Dim, W);
        break;
    }
    case AssetKind::Fx:      // sparkles
    {
        const auto Star = [&](ImVec2 O, float K) {
            const ImVec2 Pts[] = { ImVec2(O.x, O.y - K), ImVec2(O.x + K * 0.22f, O.y - K * 0.22f), ImVec2(O.x + K, O.y), ImVec2(O.x + K * 0.22f, O.y + K * 0.22f),
                                   ImVec2(O.x, O.y + K), ImVec2(O.x - K * 0.22f, O.y + K * 0.22f), ImVec2(O.x - K, O.y), ImVec2(O.x - K * 0.22f, O.y - K * 0.22f) };
            DL->AddConvexPolyFilled(Pts, 8, Col);
        };
        Star(P(-0.15f, 0.1f), R * 0.95f);
        Star(P(0.7f, -0.6f), R * 0.4f);
        break;
    }
    default:
        DL->AddCircle(C, R, Col, 24, W);
        break;
    }
}

AssetGrid::AssetGrid(ID3D11Device* Dev, ID3D11DeviceContext* Ctx)
    : Services(std::make_unique<GridServices>(Dev, Ctx)), Models(std::make_shared<ModelCellRenderer>())
{
    Renderers[AssetKind::Model] = Models;
    Renderers[AssetKind::Anim] = Models;
    Renderers[AssetKind::Map] = Models;
    Renderers[AssetKind::Image] = MakeImageCells();
    Renderers[AssetKind::Audio] = MakeAudioCells();
    Renderers[AssetKind::Fx] = MakeIconCells();
    SetCellSize((float)std::atof(SettingsManager::GetSetting("assets_grid_cell", "136").c_str()));
}

AssetGrid::~AssetGrid()
{
    Services.reset();   // the workers first: their jobs point into the renderers
}

void AssetGrid::SetRenderer(int Kind, std::shared_ptr<IAssetCellRenderer> Renderer)
{
    if (Kind >= 0 && Kind < AssetKind::Count) Renderers[Kind] = std::move(Renderer);
}

void AssetGrid::SetCellSize(float Pixels) { Size = std::clamp(std::isfinite(Pixels) ? Pixels : 136.0f, 64.0f, 384.0f); }

void AssetGrid::SetAnimRig(const std::string& ModelPath) { Models->SetRig(ModelPath); }

void AssetGrid::SetAnimRigResolver(std::function<bool(const std::string&, std::vector<std::string>&)> Resolve) { Models->SetRigResolver(std::move(Resolve)); }

void AssetGrid::SetTurning(bool On) { Models->SetTurning(On); }

void AssetGrid::SetLighting(const PreviewLighting& L) { Models->SetLighting(L); }

void AssetGrid::DrawSizeSlider()
{
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    ImGui::SliderFloat("##gridcell", &Size, 64.0f, 384.0f, "cells %.0f px");
    if (ImGui::IsItemDeactivatedAfterEdit()) SettingsManager::SetSetting("assets_grid_cell", std::to_string((int)Size));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cell size (Ctrl+wheel over the grid)");
}

void AssetGrid::Draw(int Count, const std::function<AssetGridItem(int)>& Item, const std::function<bool(int)>& Selected,
                     const std::function<void(int)>& OnItem, int ScrollTo)
{
    Services->Jobs.DrainUi();
    Frame++;
    Services->BeginFrame(Frame);

    // each renderer once, whatever number of kinds it serves
    std::vector<IAssetCellRenderer*> Unique;
    for (const auto& R : Renderers)
        if (R && std::find(Unique.begin(), Unique.end(), R.get()) == Unique.end()) Unique.push_back(R.get());

    ImGuiIO& Io = ImGui::GetIO();
    const float Em = ImGui::GetFontSize();
    const float Gap = std::round(Em * 0.45f), Pad = std::round(Em * 0.3f);
    // as many cells of Size as fit beside the scrollbar, widened to fill the row
    const float Outer = std::max(1.0f, ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize);
    const int Cols = std::max(1, (int)((Outer + Gap) / (std::round(Size) + Gap)));
    const float CellW = std::max(16.0f, std::floor((Outer - (Cols - 1) * Gap) / Cols));
    const float Thumb = CellW - 2 * Pad;
    const float TextH = ImGui::GetTextLineHeight();
    const float CellH = Pad + Thumb + Pad * 0.6f + TextH * 2 + Pad;
    const int ThumbPixels = std::clamp(((int)std::ceil(Thumb) + 15) / 16 * 16, 32, 512);
    AssetCellContext X{ *Services, ImGui::GetTime(), Frame, ThumbPixels };
    for (auto* R : Unique) R->BeginFrame(X);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Gap, Gap));
    const bool CtrlWheel = Io.KeyCtrl;
    if (ImGui::BeginChild("##assetgrid", ImVec2(0, 0), ImGuiChildFlags_None, CtrlWheel ? ImGuiWindowFlags_NoScrollWithMouse : 0))
    {
        if (CtrlWheel && Io.MouseWheel != 0.0f && ImGui::IsWindowHovered())
        {
            SetCellSize(Size * (Io.MouseWheel > 0 ? 1.1f : 1.0f / 1.1f));
            SettingsManager::SetSetting("assets_grid_cell", std::to_string((int)Size));
        }
        const float Avail = ImGui::GetContentRegionAvail().x;
        if (AutoScroll != 0) ImGui::SetScrollY(ImGui::GetScrollY() + AutoScroll * Io.DeltaTime);
        const int Rows = (Count + Cols - 1) / Cols;
        LastColumns = Cols;

        if (ScrollTo >= 0 && ScrollTo < Count)
        {
            const float Y = (ScrollTo / Cols) * (CellH + Gap), Top = ImGui::GetScrollY(), View = ImGui::GetWindowHeight();
            if (Y < Top) ImGui::SetScrollY(Y);
            else if (Y + CellH > Top + View) ImGui::SetScrollY(Y + CellH - View + Gap);
        }

        ImDrawList* DL = ImGui::GetWindowDrawList();
        const ImU32 ColSel = ImGui::GetColorU32(ImGuiCol_Header), ColHover = ImGui::GetColorU32(ImGuiCol_HeaderHovered, 0.45f);
        const ImU32 ColCell = IM_COL32(255, 255, 255, 8), ColName = ImGui::GetColorU32(ImGuiCol_Text), ColKind = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        const ImU32 ColGame = IM_COL32(120, 175, 235, 255);
        int Visible = 0;
        ImGuiListClipper Clipper;
        Clipper.Begin(Rows, CellH + Gap);
        while (Clipper.Step())
            for (int Row = Clipper.DisplayStart; Row < Clipper.DisplayEnd; Row++)
            {
                const ImVec2 RowPos = ImGui::GetCursorScreenPos();
                for (int Col = 0; Col < Cols; Col++)
                {
                    const int i = Row * Cols + Col;
                    if (i >= Count) break;
                    const AssetGridItem A = Item(i);
                    if (!A.Path) continue;
                    const ImVec2 Min(RowPos.x + Col * (CellW + Gap), RowPos.y), Max(Min.x + CellW, Min.y + CellH);
                    ImGui::SetCursorScreenPos(Min);
                    ImGui::PushID(A.Id);
                    ImGui::InvisibleButton("##cell", ImVec2(CellW, CellH));
                    const bool Hovered = ImGui::IsItemHovered();
                    const bool IsSel = Selected(i);
                    DL->AddRectFilled(Min, Max, IsSel ? ColSel : Hovered ? ColHover : ColCell, Em * 0.3f);

                    const ImVec2 TMin(Min.x + Pad, Min.y + Pad), TMax(TMin.x + Thumb, TMin.y + Thumb);
                    IAssetCellRenderer* R = A.Kind >= 0 && A.Kind < AssetKind::Count ? Renderers[A.Kind].get() : nullptr;
                    DL->PushClipRect(TMin, TMax, true);
                    if (R) R->Draw(X, A, DL, TMin, TMax, Hovered);
                    else DrawAssetKindIcon(DL, TMin, TMax, A.Kind);
                    DL->PopClipRect();
                    Visible++;

                    const float TextX0 = Min.x + Pad, TextX1 = Max.x - Pad;
                    const float NameY = TMax.y + Pad * 0.6f;
                    const char* Name = A.Name ? A.Name->c_str() : "";
                    ImGui::RenderTextEllipsis(DL, ImVec2(TextX0, NameY), ImVec2(TextX1, NameY + TextH), TextX1, Name, nullptr, nullptr);
                    // the game first (one asset of two games is two cells alike otherwise), then the kind
                    float KindX = TextX0;
                    if (A.Game && !A.Game->empty())
                    {
                        const std::string GameText = *A.Game + "  \xC2\xB7  ";
                        ImGui::PushStyleColor(ImGuiCol_Text, ColGame);
                        ImGui::RenderTextEllipsis(DL, ImVec2(TextX0, NameY + TextH), ImVec2(TextX1, NameY + 2 * TextH), TextX1, GameText.c_str(), nullptr, nullptr);
                        ImGui::PopStyleColor();
                        KindX = std::min(TextX1, TextX0 + ImGui::CalcTextSize(GameText.c_str()).x);
                    }
                    std::string KindText = AssetKindName(A.Kind);
                    if (A.Format && !A.Format->empty()) KindText += "  \xC2\xB7  " + *A.Format;
                    ImGui::PushStyleColor(ImGuiCol_Text, ColKind);
                    if (KindX < TextX1)
                        ImGui::RenderTextEllipsis(DL, ImVec2(KindX, NameY + TextH), ImVec2(TextX1, NameY + 2 * TextH), TextX1, KindText.c_str(), nullptr, nullptr);
                    ImGui::PopStyleColor();
                    (void)ColName;
                    if (Hovered && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay))
                    {
                        if (A.Game && !A.Game->empty()) ImGui::SetTooltip("%s\n%s", Name, A.Game->c_str());
                        else ImGui::SetTooltip("%s", Name);
                    }
                    OnItem(i);
                    ImGui::PopID();
                }
                ImGui::SetCursorScreenPos(RowPos);
                ImGui::Dummy(ImVec2(std::max(1.0f, Avail), CellH));
            }
        LastVisible = Visible;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    for (auto* R : Unique) R->EndFrame(X);
}

bool AssetGrid::Settled() const
{
    for (const auto& R : Renderers) if (R && !R->Settled()) return false;
    return Services->Jobs.Pending() == 0 && Services->ReadBacksPending() == 0;
}

std::string AssetGrid::Stats() const
{
    std::string S = "visible cells " + std::to_string(LastVisible) + ", columns " + std::to_string(LastColumns) +
                    ", jobs pending " + std::to_string(Services->Jobs.Pending());
    std::vector<IAssetCellRenderer*> Seen;
    for (const auto& R : Renderers)
    {
        if (!R || std::find(Seen.begin(), Seen.end(), R.get()) != Seen.end()) continue;
        Seen.push_back(R.get());
        if (const std::string Line = R->Stats(); !Line.empty()) S += "\n" + Line;
    }
    return S;
}
