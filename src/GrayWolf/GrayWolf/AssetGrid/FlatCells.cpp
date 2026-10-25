#include "stdafx.h"

#include "CellRenderers.h"
#include "GridServices.h"
#include "AudioPlayer.h"       // ReadWaveform

#include "imgui.h"

#include <algorithm>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr int ImageThumbSize = 256;
    constexpr int WaveColumns = 96;

    // A cache of one result per asset path, the least recently seen dropped past Max (never one drawn
    // in the last frame: its texture may still be in ImGui's draw data).
    template <class T> struct SeenCache
    {
        struct Entry { T Value; uint64_t SeenFrame = 0; double LastSeen = 0; bool Ready = false; };
        std::unordered_map<std::string, Entry> Map;
        size_t Max;
        explicit SeenCache(size_t M) : Max(M) {}

        Entry& Touch(const std::string& Key, uint64_t Frame, double Time)
        {
            Entry& E = Map[Key];
            E.SeenFrame = Frame; E.LastSeen = Time;
            return E;
        }
        Entry* Find(const std::string& Key) { const auto It = Map.find(Key); return It == Map.end() ? nullptr : &It->second; }
        void Trim(uint64_t Frame)
        {
            if (Map.size() <= Max) return;
            std::vector<std::pair<double, std::string>> Old;
            for (const auto& [K, E] : Map) if (E.SeenFrame + 1 < Frame) Old.emplace_back(E.LastSeen, K);
            std::sort(Old.begin(), Old.end());
            for (size_t i = 0; i < Old.size() && Map.size() > Max * 3 / 4; i++) Map.erase(Old[i].second);
        }
    };

    void DrawSpinner(ImDrawList* DL, const ImVec2& Min, const ImVec2& Max, double Time)
    {
        const float R = std::max(4.0f, (Max.x - Min.x) * 0.06f);
        const ImVec2 C(Max.x - R * 2.0f, Max.y - R * 2.0f);
        const float T = (float)Time * 6.0f;
        DL->PathArcTo(C, R, T, T + 4.2f, 16);
        DL->PathStroke(IM_COL32(120, 170, 230, 220), 0, 2.0f);
    }

    class ImageCells : public IAssetCellRenderer
    {
        struct Thumb { ComPtr<ID3D11ShaderResourceView> Srv; int W = 0, H = 0; bool Failed = false; };
        SeenCache<Thumb> Cache{ 2048 };
        size_t Visible = 0, Waiting = 0, LastWaiting = 0;

    public:
        void BeginFrame(AssetCellContext& X) override { Cache.Trim(X.Frame); Visible = Waiting = 0; }

        void Draw(AssetCellContext& X, const AssetGridItem& A, ImDrawList* DL, const ImVec2& Min, const ImVec2& Max, bool) override
        {
            auto& E = Cache.Touch(*A.Path, X.Frame, X.Time);
            Visible++;
            if (!E.Ready)
            {
                Waiting++;
                GridServices* S = &X.Services;
                const std::string Path = *A.Path, Ext = A.Format ? *A.Format : std::string(), File = ThumbCache::FileFor(A, "image");
                X.Services.Jobs.Want("image|" + Path, 0, false, [this, S, Path, Ext, File] {
                    return [this, S, Path, Ext, File](const GridJobs::StaleFn& Stale) {
                        int W = 0, H = 0; std::vector<uint8_t> Px;
                        bool Ok = ThumbCache::Load(File, W, H, Px);
                        if (!Ok && !Stale())
                        {
                            Ok = DecodeImageThumb(Path, Ext, ImageThumbSize, W, H, Px);
                            if (Ok) ThumbCache::Save(File, W, H, Px);
                        }
                        if (Stale() && !Ok) return;
                        Thumb T; T.W = W; T.H = H; T.Failed = !Ok;
                        if (Ok) T.Srv = MakeRgbaSRV(S->Dev, W, H, Px);
                        S->Jobs.PostUi([this, Path, T] { auto& E = Cache.Map[Path]; E.Value = T; E.Ready = true; });
                    };
                });
            }
            const Thumb& T = E.Value;
            if (E.Ready && T.Srv && T.W > 0 && T.H > 0)
            {
                // fitted in the square, centred
                const float Side = Max.x - Min.x, K = Side / (float)std::max(T.W, T.H);
                const float W = T.W * K, H = T.H * K;
                const ImVec2 P0(Min.x + (Side - W) * 0.5f, Min.y + (Side - H) * 0.5f);
                DL->AddImage((ImTextureID)(intptr_t)T.Srv.Get(), P0, ImVec2(P0.x + W, P0.y + H));
            }
            else
            {
                DrawAssetKindIcon(DL, Min, Max, AssetKind::Image);
                if (!E.Ready) DrawSpinner(DL, Min, Max, X.Time);
            }
        }

        void EndFrame(AssetCellContext&) override { LastWaiting = Waiting; }
        bool Settled() const override { return LastWaiting == 0; }
        std::string Stats() const override { return "images visible " + std::to_string(Visible) + ", decoded in memory " + std::to_string(Cache.Map.size()); }
    };

    class AudioCells : public IAssetCellRenderer
    {
        struct Wave { std::vector<float> Lo, Hi; double Seconds = 0; bool Failed = false; };
        SeenCache<std::shared_ptr<const Wave>> Cache{ 8192 };
        size_t Waiting = 0, LastWaiting = 0;

    public:
        void BeginFrame(AssetCellContext& X) override { Cache.Trim(X.Frame); Waiting = 0; }

        void Draw(AssetCellContext& X, const AssetGridItem& A, ImDrawList* DL, const ImVec2& Min, const ImVec2& Max, bool) override
        {
            auto& E = Cache.Touch(*A.Path, X.Frame, X.Time);
            if (!E.Ready)
            {
                Waiting++;
                GridServices* S = &X.Services;
                const std::string Path = *A.Path, Ext = A.Format ? *A.Format : std::string();
                X.Services.Jobs.Want("wave|" + Path, 0, false, [this, S, Path, Ext] {
                    return [this, S, Path, Ext](const GridJobs::StaleFn&) {
                        auto W = std::make_shared<Wave>();
                        W->Failed = !AudioPlayer::ReadWaveform(Path, Ext, WaveColumns, W->Lo, W->Hi, &W->Seconds);
                        S->Jobs.PostUi([this, Path, W] { auto& E = Cache.Map[Path]; E.Value = W; E.Ready = true; });
                    };
                });
            }
            const Wave* W = E.Ready ? E.Value.get() : nullptr;
            if (!W || W->Failed || W->Lo.empty())
            {
                DrawAssetKindIcon(DL, Min, Max, AssetKind::Audio);
                if (!E.Ready) DrawSpinner(DL, Min, Max, X.Time);
                return;
            }
            DL->AddRectFilled(Min, Max, IM_COL32(28, 31, 38, 255));
            const float Side = Max.x - Min.x, Pad = Side * 0.08f;
            const float MidY = (Min.y + Max.y) * 0.5f, Amp = Side * 0.36f;
            const int N = (int)W->Lo.size();
            const float Step = (Side - 2 * Pad) / N;
            for (int i = 0; i < N; i++)
            {
                const float X0 = Min.x + Pad + i * Step;
                const float Y0 = MidY - std::max(W->Hi[i], 0.01f) * Amp, Y1 = MidY - std::min(W->Lo[i], -0.01f) * Amp;
                DL->AddRectFilled(ImVec2(X0, Y0), ImVec2(X0 + std::max(1.0f, Step * 0.7f), Y1), IM_COL32(120, 170, 220, 220));
            }
            char Len[32];
            const int Sec = (int)W->Seconds;
            snprintf(Len, sizeof(Len), "%d:%02d.%d", Sec / 60, Sec % 60, (int)((W->Seconds - Sec) * 10));
            DL->AddText(ImVec2(Min.x + Pad * 0.5f, Max.y - ImGui::GetFontSize() - Pad * 0.3f), IM_COL32(200, 200, 205, 200), Len);
        }

        void EndFrame(AssetCellContext&) override { LastWaiting = Waiting; }
        bool Settled() const override { return LastWaiting == 0; }
    };

    class IconCells : public IAssetCellRenderer
    {
    public:
        void Draw(AssetCellContext&, const AssetGridItem& A, ImDrawList* DL, const ImVec2& Min, const ImVec2& Max, bool) override
        {
            DrawAssetKindIcon(DL, Min, Max, A.Kind);
        }
    };
}

std::shared_ptr<IAssetCellRenderer> MakeImageCells() { return std::make_shared<ImageCells>(); }
std::shared_ptr<IAssetCellRenderer> MakeAudioCells() { return std::make_shared<AudioCells>(); }
std::shared_ptr<IAssetCellRenderer> MakeIconCells() { return std::make_shared<IconCells>(); }
