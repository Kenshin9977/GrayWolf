#include "stdafx.h"

#include "Games/Common/CipherFit.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <numeric>
#include <thread>
#include <unordered_map>
#include <unordered_set>

// The prototype's stages (cipher_from_zones/solve.py), each cell being a (slot, position) of a
// slot's strings: 1 unigram then bigram fit per cell; 2 the hash check picks among a cell's best
// candidates, the model retrained on the strings that check; 3 slots whose confident cells agree
// merged; 4 the cipher's structure (one k, or the k -> r map) learned and imposed; 5 a trigram.

namespace
{
    constexpr int Cands = 2048;          // (r, k): r * 256 + k
    constexpr int TopK = 24;
    constexpr int Boundary = 256;        // start / end of a text in the models, past the end in a decode
    constexpr int MaxGroup = 16;         // slots one merged cipher may cover
    constexpr int ConfSamples = 12;      // no hash check: a cell with that many strings is trusted
    constexpr uint64_t FnvPrime = 0x100000001B3ull, FnvBasis = 0xCBF29CE484222325ull;

    // Hand-written identifier words of the kind the strings are (bones, notetracks, sounds): the
    // first character model, before the strings that check retrain it
    constexpr const char* SeedWords[] = {
        "tag_origin", "tag_weapon_right", "tag_weapon_left", "tag_flash", "tag_brass", "tag_clip", "tag_sight_on",
        "tag_camera", "tag_eye", "tag_player", "j_spine4", "j_spineupper", "j_spinelower", "j_mainroot", "j_head",
        "j_neck", "j_shoulder_le", "j_shoulder_ri", "j_elbow_le", "j_elbow_ri", "j_wrist_le", "j_wrist_ri", "j_hip_le",
        "j_hip_ri", "j_knee_le", "j_knee_ri", "j_ankle_le", "j_ankle_ri", "j_ball_le", "j_ball_ri", "j_gun",
        "j_clavicle_le", "j_index_le_1", "j_mid_ri_2", "reload_start", "reload_end", "reload_empty", "fire", "fire_ads",
        "sprint_loop", "sprint_in", "sprint_out", "raise", "drop", "idle", "walk", "run", "crouch_walk", "prone_crawl",
        "inspect", "melee", "equip", "ads_up", "ads_down", "ps_weap_reload_mag_out", "ps_weap_reload_mag_in",
        "ps_foley_cloth", "end", "start", "loop", "attach", "detach", "fx_muzzle_flash", "vfx_explosion_small",
        "scr_notify", "anim_end", "mag_out", "mag_in", "bolt_back", "bolt_forward", "chamber", "weapon_ar_01",
        "mp_player_body", "vm_weapon_rifle", "door_open", "door_close", "light_on", "light_off",
    };

    struct ByteMap
    {
        uint8_t M[Cands][256];
        ByteMap()
        {
            for (int N = 0; N < Cands; N++)
                for (int C = 0; C < 256; C++)
                {
                    const int R = N >> 8, K = N & 255;
                    const uint8_t V = (uint8_t)(C != K ? C ^ K : C);
                    M[N][C] = (uint8_t)(R ? (V << R) | (V >> (8 - R)) : V);
                }
        }
    };
    const ByteMap& Map()
    {
        static const auto M = std::make_unique<ByteMap>();
        return *M;
    }

    uint64_t LowerByte(int C) { return (uint64_t)(C >= 'A' && C <= 'Z' ? C + 32 : C); }
    int B1Of(uint64_t H) { return (int)((H >> 32) & 0xFF); }

    bool Never(int C) { return (C < 0x20 && C != 9 && C != 10 && C != 13) || C == 0x7F; }

    // unigram and bigram log probabilities of plaintext bytes, Boundary the start and end
    struct CharModel
    {
        std::array<float, 256> Uni{};
        std::vector<float> Bi;   // [257][257]
        std::vector<float> ST;   // [256][Cands]: Uni of the plaintext candidate n makes of byte c

        explicit CharModel(const std::vector<std::string>& Corpus)
        {
            constexpr double Alpha = 0.1, Bad = -30.0;
            std::vector<double> U(256, Alpha), B(257 * 257, Alpha);
            for (const auto& S : Corpus)
            {
                int Prev = Boundary;
                for (const char Ch : S)
                {
                    const int C = (uint8_t)Ch;
                    U[C] += 1;
                    B[Prev * 257 + C] += 1;
                    Prev = C;
                }
                B[Prev * 257 + Boundary] += 1;
            }
            const double Sum = std::accumulate(U.begin(), U.end(), 0.0);
            for (int C = 0; C < 256; C++) Uni[C] = Never(C) ? (float)Bad : (float)std::log(U[C] / Sum);
            Bi.resize(257 * 257);
            for (int A = 0; A < 257; A++)
            {
                double Row = 0;
                for (int C = 0; C < 257; C++) Row += B[A * 257 + C];
                for (int C = 0; C < 257; C++)
                    Bi[A * 257 + C] = (A < 256 && Never(A)) || (C < 256 && Never(C)) ? (float)Bad : (float)std::log(B[A * 257 + C] / Row);
            }
            ST.resize((size_t)256 * Cands);
            const auto& M = Map();
            for (int N = 0; N < Cands; N++)
                for (int C = 0; C < 256; C++) ST[(size_t)C * Cands + N] = Uni[M.M[N][C]];
        }
    };

    // log P(c | a, b), interpolated with the bigram's P(c | b)
    struct Trigram
    {
        std::vector<float> Lp;   // [257][257][257]
        Trigram(const std::vector<std::string>& Corpus, const CharModel& Bigram)
        {
            constexpr float Beta = 2.0f, Bad = -30.0f;
            constexpr size_t N = 257;
            Lp.assign(N * N * N, 0.0f);
            for (const auto& S : Corpus)
            {
                size_t A = Boundary, B = Boundary;
                for (size_t I = 0; I <= S.size(); I++)
                {
                    const size_t C = I < S.size() ? (uint8_t)S[I] : Boundary;
                    Lp[(A * N + B) * N + C] += 1;
                    A = B, B = C;
                }
            }
            for (size_t A = 0; A < N; A++)
                for (size_t B = 0; B < N; B++)
                {
                    float* T = &Lp[(A * N + B) * N];
                    float Ctx = 0;
                    for (size_t C = 0; C < N; C++) Ctx += T[C];
                    for (size_t C = 0; C < N; C++)
                        T[C] = C < 256 && Never((int)C) ? Bad : std::log((T[C] + Beta * std::exp(Bigram.Bi[B * N + C])) / (Ctx + Beta));
                }
        }
        float At(int A, int B, int C) const { return Lp[((size_t)A * 257 + (size_t)B) * 257 + (size_t)C]; }
    };

    struct Slot
    {
        uint32_t Key = 0;                    // (id << 16) | group
        std::vector<uint32_t> Keys;          // every (id, group) it covers
        std::vector<uint32_t> Str;           // indices of its strings, sorted
        int L = 0, N = 0;
        std::vector<uint8_t> C;              // [N][L] ciphertext bytes past the header
        std::vector<uint16_t> Len;
        std::vector<uint8_t> B1;
        std::vector<uint16_t> Top;           // [L][TopK]
        std::vector<uint16_t> Pick;          // [L]
        std::vector<uint8_t> Locked;         // [L]
        std::vector<int> Support;            // [L]
        std::vector<uint8_t> Ok;             // [N]
        const std::vector<uint8_t>* Row = nullptr;   // the candidates the structure allows, nullptr: any

        bool Allowed(int I, int Cand) const
        {
            if (Row == nullptr) return true;
            if (Locked[I]) return Cand == Pick[I];
            return (*Row)[Cand] != 0;
        }
        uint8_t Cipher(int J, int I) const { return C[(size_t)J * L + I]; }
    };

    Slot MakeSlot(uint32_t Key, std::vector<uint32_t> Idx, const std::vector<std::string>& S)
    {
        Slot Sl;
        Sl.Key = Key;
        Sl.Keys = { Key };
        Sl.Str = std::move(Idx);
        Sl.N = (int)Sl.Str.size();
        for (const auto I : Sl.Str) Sl.L = std::max(Sl.L, (int)S[I].size() - 3);
        Sl.C.assign((size_t)Sl.N * Sl.L, 0);
        Sl.Len.resize(Sl.N);
        Sl.B1.resize(Sl.N);
        for (int J = 0; J < Sl.N; J++)
        {
            const auto& T = S[Sl.Str[J]];
            Sl.Len[J] = (uint16_t)(T.size() - 3);
            Sl.B1[J] = (uint8_t)T[1];
            std::memcpy(&Sl.C[(size_t)J * Sl.L], T.data() + 3, T.size() - 3);
        }
        Sl.Locked.assign(Sl.L, 0);
        Sl.Support.assign(Sl.L, 0);
        Sl.Ok.assign(Sl.N, 0);
        return Sl;
    }

    std::vector<int16_t> Decode(const Slot& Sl)
    {
        const auto& M = Map();
        std::vector<int16_t> D((size_t)Sl.N * Sl.L, Boundary);
        for (int J = 0; J < Sl.N; J++)
            for (int I = 0; I < Sl.Len[J]; I++) D[(size_t)J * Sl.L + I] = M.M[Sl.Pick[I]][Sl.Cipher(J, I)];
        return D;
    }

    class Fitter
    {
    public:
        Fitter(const std::vector<std::string>& S, const CipherFit::Options& O) : S(S), O(O)
        {
            Threads = O.Threads ? O.Threads : std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
            for (const char* W : SeedWords) Seeds.emplace_back(W);
            First = Seeds;
            for (const auto& W : O.Corpus)
                if (!W.empty() && W.size() < 254) First.push_back(W);
            if (!O.HashChecked) Known.insert(First.begin(), First.end());
            // (the later models blend at most BlendNames of them, spread over the corpus)
            const size_t Step = std::max<size_t>(1, O.Corpus.size() / BlendNames);
            for (size_t I = 0; I < O.Corpus.size(); I += Step)
                if (!O.Corpus[I].empty() && O.Corpus[I].size() < 254) Seeds.push_back(O.Corpus[I]);
        }

        bool Run(CipherFit::Result& Out);

    private:
        const std::vector<std::string>& S;
        const CipherFit::Options& O;
        unsigned Threads = 1;
        static constexpr size_t BlendNames = 20000;
        std::vector<std::string> Seeds;   // the hand-written words and a share of the corpus
        std::vector<std::string> First;   // the first model's: the words and the whole corpus
        std::unordered_set<std::string> Known;   // no hash check: the names a decryption may be (First)
        std::vector<Slot> Slots;
        std::array<uint16_t, 256> GroupOf{};   // b1's group
        int Groups = 0;
        std::array<int, 256> R{};
        std::vector<std::unique_ptr<std::vector<uint8_t>>> Rows;   // the structure's allowed rows

        bool Cancelled() const { return O.Cancel != nullptr && O.Cancel->load(); }
        void Log(const std::string& Line) const
        {
            if (O.Log) O.Log(Line);
        }

        template <class F>
        void Each(std::vector<Slot>& V, F&& Fn) const
        {
            std::atomic<size_t> Next{ 0 };
            const auto Work = [&] {
                for (size_t I; (I = Next++) < V.size();)
                    if (!Cancelled()) Fn(V[I]);
            };
            std::vector<std::thread> Ts;
            for (unsigned T = 1; T < Threads && T < V.size(); T++) Ts.emplace_back(Work);
            Work();
            for (auto& T : Ts) T.join();
        }

        std::vector<Slot> MakeGroups(const std::array<uint16_t, 256>& Of) const
        {
            std::map<uint32_t, std::vector<uint32_t>> G;
            for (uint32_t I = 0; I < S.size(); I++) G[((uint32_t)(uint8_t)S[I][0] << 16) | Of[(uint8_t)S[I][1]]].push_back(I);
            std::vector<Slot> Out;
            Out.reserve(G.size());
            for (auto& [K, V] : G) Out.push_back(MakeSlot(K, std::move(V), S));
            return Out;
        }

        static void Unigram(Slot& Sl, const CharModel& Lm, bool OnlyUnlocked);
        static size_t Bigram(Slot& Sl, const CharModel& Lm, int Rounds);
        static size_t Trigrams(Slot& Sl, const Trigram& Tg, int Rounds);
        static size_t Refine(Slot& Sl, int K, int Rounds, int Weak);
        size_t KnownRefine(Slot& Sl, int K, int Rounds) const;
        void Crib();
        bool IsKnown(const std::string& T) const { return Known.count(T) != 0; }
        void Lock(Slot& Sl) const;
        std::vector<std::string> Verified();
        void FindGroups();
        void Merge();
        void Structure();
        std::string Table() const;

        size_t Checked() const
        {
            size_t N = 0;
            for (const auto& Sl : Slots)
                for (const auto V : Sl.Ok) N += V;
            return N;
        }
        void AfterStage(int Stage)
        {
            std::atomic<size_t> Moved{ 0 };
            if (O.HashChecked) Each(Slots, [&](Slot& Sl) { Moved += Refine(Sl, 8, 3, 3); });
            else Each(Slots, [&](Slot& Sl) { Moved += KnownRefine(Sl, 8, 3); });
            Each(Slots, [&](Slot& Sl) { Lock(Sl); });
            size_t Locked = 0;
            for (const auto& Sl : Slots)
                for (const auto V : Sl.Locked) Locked += V;
            char Line[200];
            std::snprintf(Line, sizeof(Line), "string cipher fit, stage %d: %zu slots, hash moved %zu, %zu cells locked, %zu of %zu strings checked", Stage,
                          Slots.size(), Moved.load(), Locked, Checked(), S.size());
            Log(Line);
        }
    };

    void Fitter::Unigram(Slot& Sl, const CharModel& Lm, bool OnlyUnlocked)
    {
        const bool Keep = OnlyUnlocked && !Sl.Top.empty();
        std::vector<uint16_t> Top((size_t)Sl.L * TopK), Pick(Sl.L);
        std::vector<float> Score(Cands);
        std::vector<int> Hist(256), Order(Cands);
        for (int I = 0; I < Sl.L; I++)
        {
            if (Keep && Sl.Locked[I])
            {
                std::copy_n(&Sl.Top[(size_t)I * TopK], TopK, &Top[(size_t)I * TopK]);
                Pick[I] = Sl.Pick[I];
                continue;
            }
            std::fill(Hist.begin(), Hist.end(), 0);
            for (int J = 0; J < Sl.N; J++)
                if (Sl.Len[J] > I) Hist[Sl.Cipher(J, I)]++;
            std::fill(Score.begin(), Score.end(), 0.0f);
            for (int C = 0; C < 256; C++)
            {
                if (!Hist[C]) continue;
                const float H = (float)Hist[C];
                const float* Row = &Lm.ST[(size_t)C * Cands];
                for (int N = 0; N < Cands; N++) Score[N] += H * Row[N];
            }
            if (Sl.Row != nullptr)
                for (int N = 0; N < Cands; N++)
                    if (!Sl.Allowed(I, N)) Score[N] = -1e9f;
            std::iota(Order.begin(), Order.end(), 0);
            std::partial_sort(Order.begin(), Order.begin() + TopK, Order.end(),
                              [&](int A, int B) { return Score[A] > Score[B] || (Score[A] == Score[B] && A < B); });
            for (int T = 0; T < TopK; T++) Top[(size_t)I * TopK + T] = (uint16_t)Order[T];
            Pick[I] = (uint16_t)Order[0];
        }
        Sl.Top = std::move(Top);
        Sl.Pick = std::move(Pick);
    }

    size_t Fitter::Bigram(Slot& Sl, const CharModel& Lm, int Rounds)
    {
        const auto& M = Map();
        auto D = Decode(Sl);
        const float* B = Lm.Bi.data();
        size_t Changed = 0;
        for (int Round = 0; Round < Rounds; Round++)
        {
            size_t Moved = 0;
            for (int I = 0; I < Sl.L; I++)
            {
                if (Sl.Locked[I]) continue;
                float Best = -INFINITY;
                int BestN = Sl.Pick[I];
                for (int T = 0; T < TopK; T++)
                {
                    const int N = Sl.Top[(size_t)I * TopK + T];
                    float Sc = 0;
                    if (!Sl.Allowed(I, N)) Sc = -1e9f;
                    else
                        for (int J = 0; J < Sl.N; J++)
                        {
                            if (Sl.Len[J] <= I) continue;
                            const int Dec = M.M[N][Sl.Cipher(J, I)];
                            const int Prev = I ? D[(size_t)J * Sl.L + I - 1] : Boundary;
                            const int Next = I + 1 < Sl.L ? D[(size_t)J * Sl.L + I + 1] : Boundary;
                            Sc += B[Prev * 257 + Dec] + B[Dec * 257 + Next];
                        }
                    if (Sc > Best)
                    {
                        Best = Sc;
                        BestN = N;
                    }
                }
                if (BestN != Sl.Pick[I])
                {
                    Sl.Pick[I] = (uint16_t)BestN;
                    for (int J = 0; J < Sl.N; J++)
                        if (Sl.Len[J] > I) D[(size_t)J * Sl.L + I] = M.M[BestN][Sl.Cipher(J, I)];
                    Moved++;
                }
            }
            Changed += Moved;
            if (!Moved) break;
        }
        return Changed;
    }

    // unlocked cells re-chosen with a trigram over their neighbours; a cell with few strings looks at
    // every allowed candidate, one with many at its top K; its top K re-ranked
    size_t Fitter::Trigrams(Slot& Sl, const Trigram& Tg, int Rounds)
    {
        constexpr int Few = 10;
        const auto& M = Map();
        const int W = Sl.L + 4;
        std::vector<int16_t> Dp((size_t)Sl.N * W, Boundary);
        {
            const auto D = Decode(Sl);
            for (int J = 0; J < Sl.N; J++)
                for (int I = 0; I < Sl.L; I++) Dp[(size_t)J * W + I + 2] = D[(size_t)J * Sl.L + I];
        }
        size_t Moved = 0;
        std::vector<int> Live, Cs, Order;
        std::vector<float> Sc;
        for (int Round = 0; Round < Rounds; Round++)
        {
            const size_t Before = Moved;
            for (int I = 0; I < Sl.L; I++)
            {
                if (Sl.Locked[I]) continue;
                Live.clear();
                for (int J = 0; J < Sl.N; J++)
                    if (Sl.Len[J] > I) Live.push_back(J);
                Cs.clear();
                if ((int)Live.size() <= Few)
                {
                    for (int N = 0; N < Cands; N++)
                        if (Sl.Allowed(I, N)) Cs.push_back(N);
                }
                else
                    for (int T = 0; T < TopK; T++)
                        if (const int N = Sl.Top[(size_t)I * TopK + T]; Sl.Allowed(I, N)) Cs.push_back(N);
                if (Cs.size() < 2) continue;
                Sc.assign(Cs.size(), 0.0f);
                for (size_t K = 0; K < Cs.size(); K++)
                {
                    float V = 0;
                    for (const int J : Live)
                    {
                        const int16_t* Row = &Dp[(size_t)J * W + I];
                        const int Dec = M.M[Cs[K]][Sl.Cipher(J, I)];
                        V += Tg.At(Row[0], Row[1], Dec) + Tg.At(Row[1], Dec, Row[3]);
                        if (Sl.Len[J] > I + 1) V += Tg.At(Dec, Row[3], Row[4]);
                    }
                    Sc[K] = V;
                }
                Order.resize(Cs.size());
                std::iota(Order.begin(), Order.end(), 0);
                const size_t Keep = std::min<size_t>(TopK, Cs.size());
                std::partial_sort(Order.begin(), Order.begin() + (std::ptrdiff_t)Keep, Order.end(),
                                  [&](int A, int B) { return Sc[A] > Sc[B] || (Sc[A] == Sc[B] && A < B); });
                for (size_t T = 0; T < Keep; T++) Sl.Top[(size_t)I * TopK + T] = (uint16_t)Cs[Order[T]];
                const int Best = Cs[Order[0]];
                if (Best != Sl.Pick[I])
                {
                    Sl.Pick[I] = (uint16_t)Best;
                    for (const int J : Live) Dp[(size_t)J * W + I + 2] = M.M[Best][Sl.Cipher(J, I)];
                    Moved++;
                }
            }
            if (Moved == Before) break;
        }
        return Moved;
    }

    // The hash check: per cell, the candidate among its top K making the most strings of the slot
    // hash to their b1. A string checks its byte i strongly when Weak bytes at least follow it
    // (FNV-1a's last multiplies barely reach bits 32-39).
    size_t Fitter::Refine(Slot& Sl, int K, int Rounds, int Weak)
    {
        const auto& M = Map();
        auto D = Decode(Sl);
        const int L = Sl.L, W = L + 1;
        std::vector<uint64_t> H((size_t)Sl.N * W);
        const auto Rehash = [&](int J, int From) {
            uint64_t* Hj = &H[(size_t)J * W];
            if (From == 0) Hj[0] = FnvBasis;
            for (int T = From; T < L; T++) Hj[T + 1] = (Hj[T] ^ LowerByte(D[(size_t)J * L + T])) * FnvPrime;
        };
        std::vector<uint8_t> Ok(Sl.N);
        for (int J = 0; J < Sl.N; J++)
        {
            Rehash(J, 0);
            Ok[J] = B1Of(H[(size_t)J * W + Sl.Len[J]]) == Sl.B1[J];
        }
        size_t Moved = 0;
        std::vector<int> Cs;
        std::vector<uint8_t> NewCol;
        std::vector<int> Strong, Hits;
        for (int Round = 0; Round < Rounds; Round++)
        {
            const size_t Before = Moved;
            for (int I = 0; I < L; I++)
            {
                bool Wanting = false;
                int OkStrong = 0;
                for (int J = 0; J < Sl.N; J++)
                    if (Sl.Len[J] > I + Weak)
                    {
                        if (!Ok[J]) Wanting = true;
                        else OkStrong++;
                    }
                if (!Wanting) continue;
                Cs.clear();
                for (int T = 0; T < TopK && (int)Cs.size() < K; T++)
                    if (const int N = Sl.Top[(size_t)I * TopK + T]; Sl.Allowed(I, N)) Cs.push_back(N);
                if (Cs.size() < 2) continue;
                const int Nc = (int)Cs.size();
                NewCol.assign((size_t)Nc * Sl.N, 0);
                Strong.assign(Nc, 0);
                Hits.assign(Nc, 0);
                for (int Kc = 0; Kc < Nc; Kc++)
                    for (int J = 0; J < Sl.N; J++)
                    {
                        if (Sl.Len[J] <= I) continue;
                        const uint8_t V = M.M[Cs[Kc]][Sl.Cipher(J, I)];
                        NewCol[(size_t)Kc * Sl.N + J] = V;
                        const bool IsStrong = Sl.Len[J] > I + Weak;
                        bool Hit;
                        if (Ok[J] && IsStrong) Hit = V == D[(size_t)J * L + I];
                        else
                        {
                            uint64_t Hv = (H[(size_t)J * W + I] ^ LowerByte(V)) * FnvPrime;
                            for (int T = I + 1; T < Sl.Len[J]; T++) Hv = (Hv ^ LowerByte(D[(size_t)J * L + T])) * FnvPrime;
                            Hit = B1Of(Hv) == Sl.B1[J];
                        }
                        if (Hit)
                        {
                            Hits[Kc]++;
                            if (IsStrong) Strong[Kc]++;
                        }
                    }
                int Best = 0, Cur = -1;
                for (int Kc = 0; Kc < Nc; Kc++)
                {
                    if (Strong[Kc] * 1000 + Hits[Kc] > Strong[Best] * 1000 + Hits[Best]) Best = Kc;
                    if (Cs[Kc] == Sl.Pick[I]) Cur = Kc;
                }
                if (Cur >= 0 && Strong[Best] * 1000 + Hits[Best] <= Strong[Cur] * 1000 + Hits[Cur]) continue;
                if (Cur < 0 && Strong[Best] <= OkStrong) continue;
                Sl.Pick[I] = (uint16_t)Cs[Best];
                for (int J = 0; J < Sl.N; J++)
                {
                    if (Sl.Len[J] <= I) continue;
                    D[(size_t)J * L + I] = NewCol[(size_t)Best * Sl.N + J];
                    Rehash(J, I);
                    Ok[J] = B1Of(H[(size_t)J * W + Sl.Len[J]]) == Sl.B1[J];
                }
                Moved++;
            }
            if (Moved == Before) break;
        }
        Sl.Ok = std::move(Ok);
        return Moved;
    }

    // No hash check (Cold War): the check is a known name instead. Per cell, the candidate among its
    // top K (the structure's, a locked cell's too: many strings lock a cell, not a right pick) making
    // the most strings of the slot decrypt to a name of the corpus; taken when it makes more, and at
    // least 2 (or the only one there)
    size_t Fitter::KnownRefine(Slot& Sl, int K, int Rounds) const
    {
        if (Known.empty()) return 0;
        const auto& M = Map();
        std::vector<std::string> Text(Sl.N);
        {
            const auto D = Decode(Sl);
            for (int J = 0; J < Sl.N; J++)
            {
                Text[J].resize(Sl.Len[J]);
                for (int I = 0; I < Sl.Len[J]; I++) Text[J][I] = (char)D[(size_t)J * Sl.L + I];
            }
        }
        size_t Moved = 0;
        std::vector<int> Cs, Live;
        for (int Round = 0; Round < Rounds; Round++)
        {
            const size_t Before = Moved;
            for (int I = 0; I < Sl.L; I++)
            {
                Live.clear();
                for (int J = 0; J < Sl.N; J++)
                    if (Sl.Len[J] > I) Live.push_back(J);
                Cs.clear();
                Cs.push_back(Sl.Pick[I]);
                for (int T = 0; T < TopK && (int)Cs.size() < K; T++)
                    if (const int N = Sl.Top[(size_t)I * TopK + T]; N != Sl.Pick[I] && (Sl.Row == nullptr || (*Sl.Row)[N])) Cs.push_back(N);
                int Best = 0, BestScore = -1, CurScore = 0;
                for (int Kc = 0; Kc < (int)Cs.size(); Kc++)
                {
                    int Score = 0;
                    for (const int J : Live)
                    {
                        std::string& T = Text[J];
                        const char Was = T[I];
                        T[I] = (char)M.M[Cs[Kc]][Sl.Cipher(J, I)];
                        Score += IsKnown(T);
                        T[I] = Was;
                    }
                    if (Kc == 0) CurScore = Score;
                    if (Score > BestScore) Best = Kc, BestScore = Score;
                }
                if (Best == 0 || BestScore <= CurScore || (BestScore < 2 && Live.size() > 1)) continue;
                Sl.Pick[I] = (uint16_t)Cs[Best];
                for (const int J : Live) Text[J][I] = (char)M.M[Cs[Best]][Sl.Cipher(J, I)];
                Moved++;
            }
            if (Moved == Before) break;
        }
        return Moved;
    }

    // No hash check: known texts as cribs. A string that is no known name, whose decryption is a few
    // bytes from exactly one text of its length (a name of the corpus, or a string another slot
    // decrypts with every cell trusted: Cold War holds one text under several headers), takes that
    // text; each cell then gets the candidate most of its strings agree on, the known and the cribbed
    // ones together, when more agree with it than with the cell's pick
    void Fitter::Crib()
    {
        const auto& M = Map();
        std::vector<std::vector<uint16_t>> Inverse(256 * 256);   // [c][plain]: the candidates turning c into plain
        for (int N = 0; N < Cands; N++)
            for (int C = 0; C < 256; C++) Inverse[(size_t)C * 256 + M.M[N][C]].push_back((uint16_t)N);
        std::vector<std::vector<std::pair<std::string, int>>> ByLen(256);   // (text, its slot; -1: the corpus)
        for (const auto& W : Known)
            if (W.size() < 256) ByLen[W.size()].push_back({ W, -1 });
        std::vector<std::vector<std::string>> Texts(Slots.size());
        for (size_t K = 0; K < Slots.size(); K++)
        {
            const Slot& Sl = Slots[K];
            const auto D = Decode(Sl);
            Texts[K].resize(Sl.N);
            for (int J = 0; J < Sl.N; J++)
            {
                std::string& T = Texts[K][J];
                T.resize(Sl.Len[J]);
                for (int I = 0; I < Sl.Len[J]; I++) T[I] = (char)D[(size_t)J * Sl.L + I];
                if (Sl.Ok[J] && !IsKnown(T)) ByLen[T.size()].push_back({ T, (int)K });
            }
        }
        std::atomic<size_t> Used{ 0 }, Moved{ 0 };
        Each(Slots, [&](Slot& Sl) {
            const size_t K = (size_t)(&Sl - Slots.data());
            std::vector<std::vector<std::pair<uint8_t, uint8_t>>> Pairs(Sl.L);   // per cell (cipher byte, plain byte)
            for (int J = 0; J < Sl.N; J++)
            {
                const std::string& T = Texts[K][J];
                const int N = (int)T.size();
                const std::string* Take = IsKnown(T) ? &T : nullptr;
                if (Take == nullptr && N >= 4)
                {
                    const int Limit = std::max(1, N / 6);
                    int BestD = Limit + 1;
                    const std::string* Best = nullptr;
                    bool Tie = false;
                    for (const auto& [Q, From] : ByLen[N])
                    {
                        if (From == (int)K) continue;
                        int D = 0;
                        for (int I = 0; I < N && D <= BestD; I++) D += Q[I] != T[I];
                        if (D == 0)
                        {
                            Best = &Q, Tie = false;
                            break;
                        }
                        if (D < BestD) Best = &Q, BestD = D, Tie = false;
                        else if (D == BestD) Tie = true;
                    }
                    if (Best != nullptr && !Tie)
                    {
                        Take = Best;
                        if (*Best != T) Used++;
                    }
                }
                if (Take != nullptr)
                    for (int I = 0; I < N; I++) Pairs[I].push_back({ Sl.Cipher(J, I), (uint8_t)(*Take)[I] });
            }
            std::vector<int> Votes(Cands, 0);
            std::vector<uint16_t> Touched;
            for (int I = 0; I < Sl.L; I++)
            {
                if (Pairs[I].empty()) continue;
                Touched.clear();
                for (const auto& [C, P] : Pairs[I])
                    for (const uint16_t N : Inverse[(size_t)C * 256 + P])
                    {
                        if (Sl.Row != nullptr && !(*Sl.Row)[N]) continue;
                        if (Votes[N]++ == 0) Touched.push_back(N);
                    }
                int Best = Sl.Pick[I];
                for (const uint16_t N : Touched)
                    if (Votes[N] > Votes[Best]) Best = N;
                if (Best != Sl.Pick[I] && Votes[Best] >= 0.6 * (double)Pairs[I].size())
                {
                    Sl.Pick[I] = (uint16_t)Best;
                    Moved++;
                }
                for (const uint16_t N : Touched) Votes[N] = 0;
            }
        });
        char Line[160];
        std::snprintf(Line, sizeof(Line), "string cipher fit: %zu strings took a known text, %zu cells moved", Used.load(), Moved.load());
        Log(Line);
    }

    // A cell is locked when a checked string covers it, not as one of its last three bytes; with no
    // hash check, when ConfSamples strings at least cover it, or a string decrypting to a known name
    void Fitter::Lock(Slot& Sl) const
    {
        std::fill(Sl.Support.begin(), Sl.Support.end(), 0);
        if (!O.HashChecked)
        {
            for (int I = 0; I < Sl.L; I++)
            {
                int Live = 0;
                for (int J = 0; J < Sl.N; J++) Live += Sl.Len[J] > I;
                Sl.Support[I] = Live >= ConfSamples ? Live : 0;
                Sl.Locked[I] = Live >= ConfSamples;
            }
            if (!Known.empty())
            {
                const auto D = Decode(Sl);
                std::string T;
                for (int J = 0; J < Sl.N; J++)
                {
                    T.assign(Sl.Len[J], '\0');
                    for (int I = 0; I < Sl.Len[J]; I++) T[I] = (char)D[(size_t)J * Sl.L + I];
                    if (!IsKnown(T)) continue;
                    for (int I = 0; I < Sl.Len[J]; I++)
                    {
                        Sl.Support[I] = std::max(Sl.Support[I], 2);
                        Sl.Locked[I] = 1;
                    }
                }
            }
            for (int J = 0; J < Sl.N; J++) Sl.Ok[J] = std::all_of(Sl.Locked.begin(), Sl.Locked.begin() + Sl.Len[J], [](uint8_t V) { return V != 0; });
            return;
        }
        const auto D = Decode(Sl);
        for (int J = 0; J < Sl.N; J++)
        {
            uint64_t H = FnvBasis;
            for (int I = 0; I < Sl.Len[J]; I++) H = (H ^ LowerByte(D[(size_t)J * Sl.L + I])) * FnvPrime;
            Sl.Ok[J] = B1Of(H) == Sl.B1[J];
            if (Sl.Ok[J])
                for (int I = 0; I < Sl.Len[J] - 3; I++) Sl.Support[I]++;
        }
        for (int I = 0; I < Sl.L; I++) Sl.Locked[I] = Sl.Support[I] > 0;
    }

    std::vector<std::string> Fitter::Verified()
    {
        std::vector<std::string> Out;
        for (const auto& Sl : Slots)
        {
            const auto D = Decode(Sl);
            for (int J = 0; J < Sl.N; J++)
                if (Sl.Ok[J])
                {
                    std::string T(Sl.Len[J], '\0');
                    for (int I = 0; I < Sl.Len[J]; I++) T[I] = (char)D[(size_t)J * Sl.L + I];
                    Out.push_back(std::move(T));
                }
        }
        Out.insert(Out.end(), Seeds.begin(), Seeds.end());
        return Out;
    }

    // The b1 grouping: a fit per (id, b1), then, for each b1, how often the fits of b1 and b1 + 1
    // (same id) agree on 3 of their first 8 positions (two different ciphers about never do): b1 + 1
    // starts a group where they mostly do not. Groups of 4 or 8 in MWIII, BO6 and BO7 ((b1 + o) >> s
    // in the prototype), of about 5 in Cold War.
    void Fitter::FindGroups()
    {
        const CharModel Lm(First);
        std::array<uint16_t, 256> Fine;
        std::iota(Fine.begin(), Fine.end(), (uint16_t)0);
        auto Slots1 = MakeGroups(Fine);
        Each(Slots1, [&](Slot& Sl) {
            Unigram(Sl, Lm, false);
            Bigram(Sl, Lm, 2);
        });
        std::map<uint32_t, const Slot*> At;
        for (const auto& Sl : Slots1) At[Sl.Key] = &Sl;
        std::array<double, 255> Rate{};
        std::array<size_t, 255> Pairs{};
        for (int B = 0; B < 255; B++)
        {
            size_t Agree = 0;
            for (int Id = 0x80; Id < 0xC0; Id++)
            {
                const auto X = At.find(((uint32_t)Id << 16) | (uint32_t)B), Y = At.find(((uint32_t)Id << 16) | (uint32_t)(B + 1));
                if (X == At.end() || Y == At.end()) continue;
                const int M = std::min({ 8, X->second->L, Y->second->L });
                int Same = 0;
                for (int T = 0; T < M; T++) Same += X->second->Pick[T] == Y->second->Pick[T];
                Pairs[B]++;
                Agree += Same >= 3;
            }
            Rate[B] = Pairs[B] ? (double)Agree / (double)Pairs[B] : 0.0;
        }
        // the agreement of neighbours in one group: the 75th percentile (a group of 2 at least)
        std::vector<double> Known;
        for (int B = 0; B < 255; B++)
            if (Pairs[B]) Known.push_back(Rate[B]);
        std::sort(Known.begin(), Known.end());
        const double High = Known.empty() ? 0.0 : Known[Known.size() * 3 / 4];
        uint16_t G = 0;
        size_t Bounds = 0;
        GroupOf[0] = 0;
        for (int B = 0; B < 255; B++)
        {
            // (no pair seen: no evidence of one cipher, a group starts)
            if (High < 0.2 || !Pairs[B] || Rate[B] < 0.5 * High) G++, Bounds++;
            GroupOf[B + 1] = G;
        }
        char Line[160];
        std::snprintf(Line, sizeof(Line), "string cipher fit: b1 in %u groups, neighbours in one group agree %.2f", (unsigned)G + 1, High);
        Log(Line);
        Groups = (int)G + 1;
    }

    // Slots whose confident cells are equal wherever both have one hold the same cipher: merged,
    // their strings pooled (two different ciphers agree on a cell by chance about 1 / 256)
    void Fitter::Merge()
    {
        constexpr int MinCommon = 6;
        const size_t Ns = Slots.size();
        std::vector<std::vector<int>> Conf(Ns);   // per slot, per position: the pick when confident, else -1
        std::unordered_map<uint32_t, size_t> ByAt;
        std::vector<std::vector<uint32_t>> By;
        for (size_t K = 0; K < Ns; K++)
        {
            const Slot& Sl = Slots[K];
            Conf[K].assign(Sl.L, -1);
            for (int I = 0; I < Sl.L; I++)
                if (Sl.Support[I] >= 2)
                {
                    Conf[K][I] = Sl.Pick[I];
                    const uint32_t Cell = ((uint32_t)I << 11) | Sl.Pick[I];
                    const auto [It, New] = ByAt.try_emplace(Cell, By.size());
                    if (New) By.emplace_back();
                    By[It->second].push_back((uint32_t)K);
                }
        }
        std::unordered_map<uint64_t, size_t> AgreeAt;
        std::vector<std::pair<uint64_t, int>> Agree;
        for (const auto& V : By)
            if (V.size() > 1 && V.size() < 50)
                for (size_t A = 0; A < V.size(); A++)
                    for (size_t B = A + 1; B < V.size(); B++)
                    {
                        const uint64_t P = ((uint64_t)V[A] << 32) | V[B];
                        const auto [It, New] = AgreeAt.try_emplace(P, Agree.size());
                        if (New) Agree.push_back({ P, 0 });
                        Agree[It->second].second++;
                    }
        std::vector<size_t> Parent(Ns), Size(Ns);
        for (size_t K = 0; K < Ns; K++) Parent[K] = K, Size[K] = Slots[K].Keys.size();
        const auto Find = [&](size_t K) {
            while (Parent[K] != K) K = Parent[K] = Parent[Parent[K]];
            return K;
        };
        size_t Merges = 0;
        for (const auto& [P, N] : Agree)
        {
            if (N < MinCommon) continue;
            const size_t A = P >> 32, B = P & 0xFFFFFFFF;
            size_t Common = 0, Bad = 0;
            const size_t M = std::min(Conf[A].size(), Conf[B].size());
            for (size_t I = 0; I < M; I++)
                if (Conf[A][I] >= 0 && Conf[B][I] >= 0)
                {
                    Common++;
                    Bad += Conf[A][I] != Conf[B][I];
                }
            if (Bad == 0 || (!O.HashChecked && (double)Bad <= 0.1 * (double)Common))
            {
                const size_t Ra = Find(A), Rb = Find(B);
                if (Ra != Rb && Size[Ra] + Size[Rb] <= MaxGroup)
                {
                    Parent[Rb] = Ra;
                    Size[Ra] += Size[Rb];
                    Merges++;
                }
            }
        }
        std::map<size_t, std::vector<size_t>> Groups;
        for (size_t K = 0; K < Ns; K++) Groups[Find(K)].push_back(K);
        std::vector<Slot> Out;
        Out.reserve(Groups.size());
        for (auto& [Root, Ks] : Groups)
        {
            if (Ks.size() == 1)
            {
                Out.push_back(std::move(Slots[Root]));
                continue;
            }
            std::vector<uint32_t> Str, Keys;
            for (const size_t K : Ks)
            {
                Str.insert(Str.end(), Slots[K].Str.begin(), Slots[K].Str.end());
                Keys.insert(Keys.end(), Slots[K].Keys.begin(), Slots[K].Keys.end());
            }
            std::sort(Str.begin(), Str.end());
            Slot Sl = MakeSlot(Slots[Root].Key, std::move(Str), S);
            Sl.Keys = std::move(Keys);
            Out.push_back(std::move(Sl));
        }
        std::sort(Out.begin(), Out.end(), [](const Slot& A, const Slot& B) { return A.Key < B.Key; });
        char Line[120];
        std::snprintf(Line, sizeof(Line), "string cipher fit: %zu merges, %zu slots -> %zu", Merges, Ns, Out.size());
        Log(Line);
        Slots = std::move(Out);
    }

    // From the confident cells: which slots keep one k, and the k -> r map the others follow (learned
    // from the slots that are not one-k)
    void Fitter::Structure()
    {
        constexpr int MinConf = 6;
        const auto ConfOf = [](const Slot& Sl) {
            std::vector<int> C;
            for (int I = 0; I < Sl.L; I++)
                if (Sl.Support[I] >= 2) C.push_back(I);
            return C;
        };
        const auto MostK = [](const Slot& Sl, const std::vector<int>& C, int& Count) {
            std::array<int, 256> N{};
            std::array<int, 256> First{};
            First.fill(INT32_MAX);
            for (size_t T = 0; T < C.size(); T++)
            {
                const int K = Sl.Pick[C[T]] & 255;
                N[K]++;
                First[K] = std::min(First[K], (int)T);
            }
            int Best = 0;
            for (int K = 1; K < 256; K++)
                if (N[K] > N[Best] || (N[K] == N[Best] && First[K] < First[Best])) Best = K;
            Count = N[Best];
            return Best;
        };
        {
            std::vector<std::array<int, 8>> Votes(256, std::array<int, 8>{});
            for (const auto& Sl : Slots)
            {
                const auto C = ConfOf(Sl);
                if ((int)C.size() < MinConf) continue;
                int Nk = 0;
                MostK(Sl, C, Nk);
                if (Nk >= 0.85 * (double)C.size()) continue;
                for (const int I : C) Votes[Sl.Pick[I] & 255][Sl.Pick[I] >> 8]++;
            }
            size_t Agree = 0, Tot = 0, Known = 0;
            for (int K = 0; K < 256; K++)
            {
                const auto& V = Votes[K];
                const int Best = (int)(std::max_element(V.begin(), V.end()) - V.begin());
                const int Sum = std::accumulate(V.begin(), V.end(), 0);
                R[K] = Sum ? Best : -1;
                Agree += Sum ? V[Best] : 0;
                Tot += Sum;
                Known += Sum != 0;
            }
            char Line[140];
            std::snprintf(Line, sizeof(Line), "string cipher fit: k -> r learned for %zu k, %zu / %zu confident cells agree", Known, Agree, Tot);
            Log(Line);
        }
        auto Many = std::make_unique<std::vector<uint8_t>>(Cands);
        for (int N = 0; N < Cands; N++) (*Many)[N] = R[N & 255] < 0 || R[N & 255] == (N >> 8);
        std::array<const std::vector<uint8_t>*, 256> OneK{};
        size_t OneKSlots = 0;
        for (auto& Sl : Slots)
        {
            Sl.Row = Many.get();
            const auto C = ConfOf(Sl);
            if ((int)C.size() < MinConf) continue;
            size_t Follow = 0;
            for (const int I : C) Follow += R[Sl.Pick[I] & 255] == (Sl.Pick[I] >> 8);
            int Nk = 0;
            const int K0 = MostK(Sl, C, Nk);
            if ((double)Follow / (double)C.size() < 0.5 && Nk >= 0.5 * (double)C.size())
            {
                if (OneK[K0] == nullptr)
                {
                    auto Row = std::make_unique<std::vector<uint8_t>>(Cands);
                    for (int N = 0; N < Cands; N++) (*Row)[N] = (N & 255) == K0;
                    OneK[K0] = Row.get();
                    Rows.push_back(std::move(Row));
                }
                Sl.Row = OneK[K0];
                OneKSlots++;
            }
        }
        Rows.push_back(std::move(Many));
        char Line[100];
        std::snprintf(Line, sizeof(Line), "string cipher fit: %zu slots with one k", OneKSlots);
        Log(Line);
    }

    std::string Fitter::Table() const
    {
        std::map<int, std::vector<int>> Select;
        for (size_t N = 0; N < Slots.size(); N++)
            for (const uint32_t K : Slots[N].Keys)
            {
                auto& Row = Select.try_emplace((int)(K >> 16), std::vector<int>(256, -1)).first->second;
                const int G = (int)(K & 0xFFFF);
                for (int B1 = 0; B1 < 256; B1++)
                    if (GroupOf[B1] == G) Row[B1] = (int)N;
            }
        std::string Out = "# string cipher rebuilt from the install's zone strings (GrayWolf, Games/Common/CipherFit.h)\n";
        char Buf[16];
        for (const auto& [Id, Row] : Select)
        {
            std::snprintf(Buf, sizeof(Buf), "select %02x", Id);
            Out += Buf;
            for (const int N : Row) Out += " " + std::to_string(N);
            Out += "\n";
        }
        for (size_t N = 0; N < Slots.size(); N++)
        {
            Out += "cipher " + std::to_string(N) + " ";
            for (int I = 0; I < Slots[N].L; I++)
            {
                const int P = Slots[N].Pick[I];
                std::snprintf(Buf, sizeof(Buf), "%x%02x", P >> 8, P & 255);
                Out += Buf;
            }
            Out += "\n";
        }
        return Out;
    }

    bool Fitter::Run(CipherFit::Result& Out)
    {
        constexpr size_t Stages = 7;
        const auto Step = [&](size_t Done) {
            if (O.Step) O.Step(Done, Stages);
            return !Cancelled();
        };
        Log("string cipher fit: " + std::to_string(S.size()) + " encrypted strings" + (O.HashChecked ? "" : ", no hash check"));
        if (!Step(0)) return false;
        FindGroups();
        if (!Step(1)) return false;
        // 1: unigram, bigram
        {
            const CharModel Lm(First);
            Slots = MakeGroups(GroupOf);
            Each(Slots, [&](Slot& Sl) {
                Unigram(Sl, Lm, false);
                Bigram(Sl, Lm, 4);
            });
            AfterStage(1);
        }
        if (!Step(2)) return false;
        // 2: bootstrap on the checked strings
        {
            const CharModel Lm(Verified());
            Each(Slots, [&](Slot& Sl) {
                Unigram(Sl, Lm, true);
                Bigram(Sl, Lm, 4);
            });
            AfterStage(2);
        }
        if (!Step(3)) return false;
        // 3: merges, then every slot refitted, its locked cells too (keeping them was measured
        // worse: MWIII 91.4 % of script strings instead of 97.9 %)
        {
            const CharModel Lm(Verified());
            Merge();
            Each(Slots, [&](Slot& Sl) {
                Unigram(Sl, Lm, false);
                Bigram(Sl, Lm, 4);
            });
            AfterStage(3);
        }
        if (!Step(4)) return false;
        // 4: structure
        {
            Structure();
            if (!O.HashChecked)
                for (auto& Sl : Slots)
                    for (int I = 0; I < Sl.L; I++)
                        if (Sl.Locked[I] && !(*Sl.Row)[Sl.Pick[I]]) Sl.Locked[I] = 0;   // a trusted cell breaking it is not
            const CharModel Lm(Verified());
            Each(Slots, [&](Slot& Sl) {
                Unigram(Sl, Lm, true);
                Bigram(Sl, Lm, 4);
            });
            AfterStage(4);
        }
        if (!Step(5)) return false;
        // 5: trigram on the checked strings
        {
            auto Vc = Verified();
            const CharModel Lm(Vc);
            Vc.resize(Vc.size() - Seeds.size());
            const auto Tg = std::make_unique<Trigram>(Vc, Lm);
            Each(Slots, [&](Slot& Sl) { Trigrams(Sl, *Tg, 3); });
            AfterStage(5);
        }
        if (!Step(6)) return false;
        // 6: no hash check: cribs, twice (Cold War: 98.8 % of its strings after one round, 98.9 % after
        // three, 15 s each; with the hash check, MWIII gains 13 strings of 274 780)
        if (!O.HashChecked)
            for (int Round = 0; Round < 2 && !Cancelled(); Round++)
            {
                Crib();
                AfterStage(6);
            }
        if (!Step(7)) return false;
        Out.Table = Table();
        Out.Strings = S.size();
        Out.Checked = Checked();
        Out.Slots = Slots.size();
        Out.Groups = Groups;
        return true;
    }
}

bool CipherFit::Encrypted(const std::string& S)
{
    return S.size() >= 4 && ((uint8_t)S[0] & 0xC0) == 0x80 && (uint8_t)S[2] == S.size() - 2;
}

bool CipherFit::Fit(const std::vector<std::string>& Strings, const Options& O, Result& Out)
{
    std::vector<std::string> S;
    for (const auto& T : Strings)
        if (Encrypted(T)) S.push_back(T);
    std::sort(S.begin(), S.end());
    S.erase(std::unique(S.begin(), S.end()), S.end());
    if (S.empty()) return false;
    Fitter F(S, O);
    return F.Run(Out);
}
