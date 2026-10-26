#include "stdafx.h"

// ui-record (UiRecord.h): a script played on the shell, one fixed time step a frame, every frame
// written as a PNG, the audio player's sound pulled at the same pace into audio.wav.
#include "ImGuiShell.Internal.h"
#include "ModelPreview.h"   // SavePreviewPng

#include "DirectXTex.h"     // CaptureTexture
#include "imgui.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace ShellUi
{
    namespace
    {
        constexpr unsigned AudioRate = 48000;

        // PNGs written off the frame (a 1080p PNG takes longer than a frame), a few frames queued at most
        class FrameWriter
        {
        public:
            FrameWriter()
            {
                for (int i = 0; i < 3; i++) Threads.emplace_back([this] { Run(); });
            }
            ~FrameWriter()
            {
                { std::lock_guard<std::mutex> L(M); Stop = true; }
                Cv.notify_all();
                for (auto& T : Threads) T.join();
            }
            void Push(std::unique_ptr<DirectX::ScratchImage> Img, std::string Path)
            {
                std::unique_lock<std::mutex> L(M);
                Room.wait(L, [this] { return Queue.size() < 8; });
                Queue.push_back({ std::move(Img), std::move(Path) });
                Cv.notify_one();
            }
            int Failed() const { return Failures.load(); }

        private:
            struct Job { std::unique_ptr<DirectX::ScratchImage> Img; std::string Path; };
            void Run()
            {
                const HRESULT Com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                for (;;)
                {
                    Job J;
                    {
                        std::unique_lock<std::mutex> L(M);
                        Cv.wait(L, [this] { return Stop || !Queue.empty(); });
                        if (Queue.empty()) break;
                        J = std::move(Queue.front());
                        Queue.pop_front();
                    }
                    Room.notify_one();
                    if (!SavePreviewPng(*J.Img->GetImage(0, 0, 0), J.Path)) Failures++;
                }
                if (SUCCEEDED(Com)) CoUninitialize();
            }
            std::mutex M;
            std::condition_variable Cv, Room;
            std::deque<Job> Queue;
            bool Stop = false;
            std::atomic<int> Failures{ 0 };
            std::vector<std::thread> Threads;
        };

        void WriteWav(const std::string& Path, const std::vector<float>& Stereo)
        {
            std::vector<int16_t> Pcm(Stereo.size());
            for (size_t i = 0; i < Stereo.size(); i++) Pcm[i] = (int16_t)std::lround(std::clamp(Stereo[i], -1.0f, 1.0f) * 32767.0f);
            const uint32_t Data = (uint32_t)(Pcm.size() * 2), Rate = AudioRate, ByteRate = AudioRate * 4;
            const uint16_t Pcm16 = 1, Channels = 2, Align = 4, Bits = 16;
            const uint32_t Riff = 36 + Data, FmtSize = 16;
            std::ofstream Out(Path, std::ios::binary);
            Out.write("RIFF", 4); Out.write((const char*)&Riff, 4); Out.write("WAVEfmt ", 8);
            Out.write((const char*)&FmtSize, 4); Out.write((const char*)&Pcm16, 2); Out.write((const char*)&Channels, 2);
            Out.write((const char*)&Rate, 4); Out.write((const char*)&ByteRate, 4); Out.write((const char*)&Align, 2); Out.write((const char*)&Bits, 2);
            Out.write("data", 4); Out.write((const char*)&Data, 4); Out.write((const char*)Pcm.data(), Data);
        }

        int TypeIndex(const std::string& Name)
        {
            static const char* Types[] = { "all", "model", "image", "audio", "anim", "map", "fx" };
            for (int i = 0; i < 7; i++) if (Name == Types[i] || Name == std::string(Types[i]) + "s") return i;
            return 0;
        }
    }

    struct UiRecorder::Impl
    {
        UiRecordOptions O;
        ComPtr<ID3D11Device> Dev;
        ComPtr<ID3D11DeviceContext> Ctx;
        nlohmann::json Steps;
        size_t Next = 0;              // the next step
        int WaitFrames = 0;           // frames still to write before it
        int Settle = 0;               // frames held after a step, the grid's cells and the views made again for it
        int Written = 0, Held = 0;
        double HoldStarted = -1;      // real seconds, while frames are held
        float OrbitRadPerSec = 0;
        nlohmann::json Camera;        // the camera step waiting for the selection to show
        bool Holding = false, Done = false, Failed = false;
        std::string Error, Timeline;
        std::vector<float> Audio;     // stereo f32 at AudioRate
        double AudioDue = 0;          // the frames of sound the video's length asks for so far
        std::unique_ptr<FrameWriter> Writer = std::make_unique<FrameWriter>();   // null once finished
        int WriteFailures = 0;

        double Now() const { return (double)GetTickCount64() / 1000.0; }

        // A selection still loading: its frames are held (written once it shows)
        bool Loading(const ShellState& S) const
        {
            const ViewportState& V = S.Viewport;
            if (S.Catalog.Walking() || S.SelectFirstModel || V.Loader.Busy() || V.AnimRigLoading) return true;
            if (S.Assets.GridView && S.Assets.Grid && !S.Assets.Grid->Settled()) return true;
            const int Kind = S.Selected >= 0 && S.Selected < (int)S.Catalog.Models.size() ? S.Catalog.Models[S.Selected].Kind : -1;
            if (Kind == 5 && !V.FxPanel.Loaded()) return true;
            return Kind == 3 && V.AnimClipOk && (V.AnimRigPaths.empty() || !V.AnimBindOk);
        }

        // the asset of that name, else the first whose name starts with it (a sound's "name.SN55.pc.snd")
        int Find(const ShellState& S, const std::string& Name, const std::string& Game) const
        {
            int Prefixed = -1;
            for (int k = 0; k < (int)S.Catalog.Models.size(); k++)
            {
                const Item& M = S.Catalog.Models[k];
                if (!Game.empty() && M.Game != Game) continue;
                if (M.Name == Name) return k;
                if (Prefixed < 0 && M.Name.rfind(Name, 0) == 0) Prefixed = k;
            }
            return Prefixed;
        }

        void Fail(const std::string& Why) { Error += Why + "\n"; Failed = true; }

        // The steps up to the next wait, all in this frame. A step's keys, applied in this order: label, type,
        // filter, view, select (+game), merge, rig, camera {yaw pitch zoom image_zoom}, orbit (deg/s), seek, play, wait (s)
        void RunSteps(ShellState& S)
        {
            while (WaitFrames <= 0 && Next < Steps.size())
            {
                const nlohmann::json& St = Steps[Next++];
                ViewportState& V = S.Viewport;
                Settle = 3;
                if (St.contains("label"))
                    Timeline += std::to_string(Written) + "\t" + St["label"].get<std::string>() + "\n";
                if (St.contains("type")) S.Assets.TypeFilter = TypeIndex(St["type"].get<std::string>());
                if (St.contains("filter")) std::snprintf(S.Assets.Filter, sizeof(S.Assets.Filter), "%s", St["filter"].get<std::string>().c_str());
                if (St.contains("view")) S.Assets.GridView = St["view"].get<std::string>() == "grid";
                if (St.contains("select"))
                {
                    const std::string Name = St["select"].get<std::string>();
                    const int k = Find(S, Name, St.value("game", std::string()));
                    if (k < 0) Fail("not listed: " + Name);
                    else { S.MultiSel.clear(); LoadSelection(S, k); S.ScrollToSelected = true; }   // a plain click
                }
                if (St.contains("merge"))   // models shown as one, as Ctrl+clicks and Merge do
                {
                    std::set<int> Picks;
                    for (const auto& Name : St["merge"])
                    {
                        const int k = Find(S, Name.get<std::string>(), St.value("game", std::string()));
                        if (k < 0) Fail("not listed: " + Name.get<std::string>());
                        else Picks.insert(k);
                    }
                    if (!Picks.empty()) { S.MultiSel = Picks; LoadMerged(S, Picks); S.ScrollToSelected = true; }
                }
                if (St.contains("rig") && S.Selected >= 0)
                {
                    const int k = Find(S, St["rig"].get<std::string>(), St.value("game", std::string()));
                    if (k < 0) Fail("not listed: " + St["rig"].get<std::string>());
                    else { S.Rigs->SetManual(S.Catalog.Models[S.Selected].Path, S.Catalog.Models[k].Path); V.AnimRigPaths.clear(); }
                }
                // set once the selection shows (a model coming in resets the camera)
                if (St.contains("camera")) Camera = St["camera"];
                if (St.contains("orbit")) OrbitRadPerSec = St["orbit"].get<float>() * 3.14159265f / 180.0f;
                if (St.contains("seek")) V.Audio.Seek(St["seek"].get<double>());
                if (St.contains("play")) { if (St["play"].get<bool>()) V.Audio.Play(); else V.Audio.Stop(); }
                if (St.contains("wait")) WaitFrames = std::max(1, (int)std::lround(St["wait"].get<double>() * O.Fps));
            }
        }
    };

    UiRecorder::UiRecorder(const UiRecordOptions& Options, ID3D11Device* Dev, ID3D11DeviceContext* Ctx) : P(std::make_unique<Impl>())
    {
        P->O = Options;
        P->Dev = Dev; P->Ctx = Ctx;
        AudioPlayer::UseOfflineClock(AudioRate);   // the player's sound pulled per frame (FrameEnd), no device
        std::error_code Ec;
        std::filesystem::create_directories(Options.OutDir, Ec);
        try
        {
            std::ifstream In(Options.Script);
            const nlohmann::json J = nlohmann::json::parse(In);
            P->Steps = J.at("steps");
            if (!P->Steps.is_array()) throw std::runtime_error("steps is not a list");
        }
        catch (const std::exception& E) { P->Fail(std::string("script: ") + E.what()); P->Done = true; }
    }

    UiRecorder::~UiRecorder() { Finish(); }

    void UiRecorder::Finish()
    {
        if (!P->Writer) return;
        P->WriteFailures = P->Writer->Failed();
        P->Writer.reset();   // the frames queued written
        if (!P->Audio.empty()) WriteWav((std::filesystem::path(P->O.OutDir) / "audio.wav").string(), P->Audio);
        std::ofstream((std::filesystem::path(P->O.OutDir) / "timeline.txt").string()) << P->Timeline;
        std::ofstream((std::filesystem::path(P->O.OutDir) / "record.txt").string()) << Summary();
    }

    void UiRecorder::FrameBegin(ShellState& S)
    {
        // the steps wait for the folder to be listed and its first selection
        if (!P->Done && P->Next == 0 && !S.Catalog.Walking() && !S.SelectFirstModel) P->RunSteps(S);
        P->Holding = P->Next > 0 && (P->Settle > 0 || P->Loading(S));
        if (P->Settle > 0) P->Settle--;
        if (P->Holding && P->HoldStarted < 0) P->HoldStarted = P->Now();
        if (!P->Holding) P->HoldStarted = -1;
        if (P->Holding && P->Now() - P->HoldStarted > P->O.HoldTimeout)
        {
            P->Fail("frame " + std::to_string(P->Written) + ": still loading after " + std::to_string((int)P->O.HoldTimeout) + " s, recorded anyway");
            P->Holding = false;
            P->HoldStarted = 1e18;   // not again for this selection
        }
        // a held frame moves time on by next to nothing (ImGui wants a step above 0)
        ImGui::GetIO().DeltaTime = P->Holding || P->Next == 0 ? 1e-5f : 1.0f / (float)P->O.Fps;
        ViewportState& V = S.Viewport;
        if (!P->Holding && P->Camera.is_object())
        {
            V.Yaw = P->Camera.value("yaw", V.Yaw); V.Pitch = P->Camera.value("pitch", V.Pitch); V.DistScale = P->Camera.value("zoom", V.DistScale);
            // the image view's zoom, kept for the image shown (a new one resets it to 100%)
            if (P->Camera.contains("image_zoom")) { V.ImageZoom = P->Camera["image_zoom"].get<float>(); V.LastZoomedImage = V.ImageSRV.Get(); }
            P->Camera = nullptr;
        }
        if (!P->Holding && P->Next > 0) V.Yaw += P->OrbitRadPerSec / (float)P->O.Fps;
    }

    bool UiRecorder::FrameEnd(ShellState& S, IDXGISwapChain* Swap)
    {
        if (P->Done) return true;
        if (P->Next == 0 || P->Holding) { P->Held++; return false; }

        auto Img = std::make_unique<DirectX::ScratchImage>();
        ComPtr<ID3D11Texture2D> Back;
        if (FAILED(Swap->GetBuffer(0, __uuidof(ID3D11Texture2D), &Back)) || FAILED(DirectX::CaptureTexture(P->Dev.Get(), P->Ctx.Get(), Back.Get(), *Img)))
        {
            P->Fail("frame capture failed");
            P->Done = true;
            return true;
        }
        char Name[32];
        std::snprintf(Name, sizeof(Name), "frame_%05d.png", P->Written);
        P->Writer->Push(std::move(Img), (std::filesystem::path(P->O.OutDir) / Name).string());
        P->Written++;

        // the sound of this frame's span, from the player's own mix
        P->AudioDue += (double)AudioRate / P->O.Fps;
        const size_t Want = (size_t)std::llround(P->AudioDue) * 2;
        if (Want > P->Audio.size())
        {
            const size_t Old = P->Audio.size();
            P->Audio.resize(Want);
            S.Viewport.Audio.ReadOffline(P->Audio.data() + Old, (Want - Old) / 2);
        }

        P->WaitFrames--;
        if (P->WaitFrames <= 0 && P->Next >= P->Steps.size()) P->Done = true;
        else P->RunSteps(S);
        return P->Done;
    }

    bool UiRecorder::Ok() const { return !P->Failed && P->WriteFailures == 0 && P->Written > 0; }

    std::string UiRecorder::Summary() const
    {
        char B[256];
        std::snprintf(B, sizeof(B), "frames=%d\nheld=%d\nfps=%d\nseconds=%.2f\nout=%s\n", P->Written, P->Held, P->O.Fps,
                      (double)P->Written / P->O.Fps, P->O.OutDir.c_str());
        return B + (P->WriteFailures ? std::to_string(P->WriteFailures) + " png writes failed\n" : std::string()) + P->Error;
    }
}
