#include "stdafx.h"

#include "AnimRigs.h"

#include "AnimOwners.h"       // anim_owners.json: the models the game says an anim plays on
#include "CastReader.h"       // CastFile::Parse, ReadAnimClip
#include "SeAnimReader.h"     // SeAnimClip
#include "DiskGame.h"         // ZoneIndex, ResolveZones: where a reference model is
#include "GameInstalls.h"     // the installs a reference model is exported from
#include "AppLog.h"
#include "Strings.h"
#include "WraithX.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>

#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace AnimRigs
{
    namespace
    {
        std::string Lower(std::string S)
        {
            for (auto& c : S) c = (char)tolower((unsigned char)c);
            return S;
        }

        // "name_LOD2" -> ("name", 2); a name with no LOD suffix is LOD 0
        std::string WithoutLod(const std::string& Stem, int* Lod = nullptr)
        {
            const auto P = Stem.rfind("_LOD");
            if (P != std::string::npos && P + 4 < Stem.size() && Stem.find_first_not_of("0123456789", P + 4) == std::string::npos)
            {
                if (Lod) *Lod = std::atoi(Stem.c_str() + P + 4);
                return Stem.substr(0, P);
            }
            if (Lod) *Lod = 0;
            return Stem;
        }

        enum class Role { Other, View, Body };

        bool Has(const std::string& S, const char* Part) { return S.find(Part) != std::string::npos; }
        bool Starts(const std::string& S, const char* Part) { return S.rfind(Part, 0) == 0; }
        bool Ends(const std::string& S, const char* Part)
        {
            const size_t N = std::strlen(Part);
            return S.size() >= N && S.compare(S.size() - N, N, Part) == 0;
        }

        Role AnimRole(const std::string& Name)
        {
            const std::string N = Lower(Name);
            if (Starts(N, "vm_") || Has(N, "viewmodel") || Has(N, "_vm_") || Ends(N, "_vm")) return Role::View;
            if (Starts(N, "pb_") || Starts(N, "mp_") || Starts(N, "pt_") || Starts(N, "ai_") || Starts(N, "sdr_") || Has(N, "body")) return Role::Body;
            return Role::Other;
        }

        Role ModelRole(const std::string& Name)
        {
            const std::string N = Lower(Name);
            if (Has(N, "viewhands") || Has(N, "viewarms") || Has(N, "view_arms") || Has(N, "view_hands") || Has(N, "vm_arms") ||
                Has(N, "viewmodel") || Has(N, "_vm_") || Ends(N, "_vm") || Has(N, "_view"))
                return Role::View;
            if (Has(N, "body") || Has(N, "_fb")) return Role::Body;
            return Role::Other;
        }

        // Name matches Pattern, '*' any run of characters (both lower case)
        bool Glob(const char* Name, const char* Pattern)
        {
            if (*Pattern == 0) return *Name == 0;
            if (*Pattern == '*') return Glob(Name, Pattern + 1) || (*Name && Glob(Name + 1, Pattern));
            return *Name == *Pattern && Glob(Name + 1, Pattern + 1);
        }
        bool AnyGlob(const std::string& Name, const std::vector<std::string>& Patterns)
        {
            for (const auto& P : Patterns) if (Glob(Name.c_str(), P.c_str())) return true;
            return false;
        }

        // A name's words worth comparing: 3 letters or more, not an anim's state ("idle", "fire")
        std::set<std::string> Words(const std::string& Name)
        {
            static const std::set<std::string> Common = {
                "viewmodel", "view", "anim", "idle", "fire", "reload", "empty", "sprint", "loop", "ads", "raise", "first",
                "pullout", "putaway", "lod0", "lod1", "lod2", "lod3", "mod", "body", "viewhands", "world", "stow", "out",
                "the", "and", "left", "right", "fast", "slow", "walk", "run", "stand", "crouch", "prone",
            };
            std::set<std::string> Out;
            std::stringstream Ss(Lower(Name));
            std::string W;
            while (std::getline(Ss, W, '_'))
                if (W.size() >= 3 && !Common.count(W)) Out.insert(W);
            return Out;
        }

        fs::path RigsRoot()
        {
            std::error_code Ec;
            const fs::path Temp = fs::temp_directory_path(Ec);
            return Ec ? fs::path() : Temp / "greyhound_rigs";
        }

        fs::path ExeDir()
        {
            wchar_t Buf[MAX_PATH * 4] = {};
            GetModuleFileNameW(nullptr, Buf, (DWORD)std::size(Buf));
            return fs::path(Buf).parent_path();
        }

        // preview_rigs.json beside the exe (the build copies it), else in a checkout above it
        fs::path TableFile()
        {
            std::error_code Ec;
            for (fs::path P = ExeDir(); !P.empty() && P != P.parent_path(); P = P.parent_path())
            {
                const fs::path F = P / "zonekit" / "profiles" / "preview_rigs.json";
                if (fs::exists(F, Ec)) return F;
            }
            return {};
        }

        bool ReadAll(const std::string& Path, std::vector<uint8_t>& Out)
        {
            std::ifstream In(fs::path(Strings::ToUnicodeString(Path)), std::ios::binary | std::ios::ate);
            if (!In) return false;
            const std::streamsize Size = In.tellg();
            if (Size < 0) return false;
            In.seekg(0);
            Out.resize((size_t)Size);
            return Size == 0 || (bool)In.read((char*)Out.data(), Size);
        }

        // Count null-terminated names from Bytes at Offset; false when the bytes end first
        bool CStrings(const std::vector<uint8_t>& Bytes, size_t Offset, uint32_t Count, std::vector<std::string>& Out)
        {
            Out.clear();
            for (uint32_t i = 0; i < Count; i++)
            {
                size_t E = Offset;
                while (E < Bytes.size() && Bytes[E] != 0) E++;
                if (E >= Bytes.size()) return false;
                Out.emplace_back((const char*)Bytes.data() + Offset, E - Offset);
                Offset = E + 1;
            }
            return true;
        }

        // A binary header's name list (SEModel, SEAnim): the u32 count at CountAt, the names from
        // NamesFrom; only as much of the file as they take is read. FlagBit unset at FlagAt: no names.
        bool ReadNames(const std::string& Path, size_t HeaderSize, size_t CountAt, size_t NamesFrom, size_t FlagAt, uint8_t FlagBit,
                       const char* Magic, std::vector<std::string>& Out)
        {
            std::ifstream In(fs::path(Strings::ToUnicodeString(Path)), std::ios::binary);
            if (!In) return false;
            std::vector<uint8_t> Bytes(HeaderSize);
            if (!In.read((char*)Bytes.data(), (std::streamsize)HeaderSize) || std::memcmp(Bytes.data(), Magic, std::strlen(Magic)) != 0) return false;
            if (FlagBit && !(Bytes[FlagAt] & FlagBit)) { Out.clear(); return true; }
            uint32_t Count = 0;
            std::memcpy(&Count, Bytes.data() + CountAt, 4);
            if (Count > 100000) return false;
            for (size_t Want = NamesFrom + (size_t)Count * 48 + 64;; Want *= 4)
            {
                const size_t Had = Bytes.size();
                Bytes.resize(Want);
                In.read((char*)Bytes.data() + Had, (std::streamsize)(Want - Had));
                Bytes.resize(Had + (size_t)In.gcount());
                if (CStrings(Bytes, NamesFrom, Count, Out)) return true;
                if (!In) return false;
            }
        }

        // An SEModel with a skeleton and no mesh ("SEModel" ver hdrSize dataFlags@11 ... meshCount@18):
        // nothing to show, no rig (MW 2019's and Black Ops 7's viewhands_base_iw8 before their mesh
        // was read from the zone holding it)
        bool MeshlessSEModel(const fs::path& Path)
        {
            std::ifstream In(Path, std::ios::binary);
            uint8_t Head[22] = {};
            uint32_t Meshes = 0;
            if (!In.read((char*)Head, sizeof(Head)) || std::memcmp(Head, "SEModel", 7) != 0) return false;
            std::memcpy(&Meshes, Head + 18, 4);
            return !(Head[11] & 2) || Meshes == 0;
        }

        struct Quat { float x = 0, y = 0, z = 0, w = 1; };

        Quat QNorm(Quat Q)
        {
            const float L = std::sqrt(Q.x * Q.x + Q.y * Q.y + Q.z * Q.z + Q.w * Q.w);
            if (L < 1e-8f) return {};
            return { Q.x / L, Q.y / L, Q.z / L, Q.w / L };
        }
        Quat QMul(const Quat& A, const Quat& B)
        {
            return { A.w * B.x + A.x * B.w + A.y * B.z - A.z * B.y, A.w * B.y - A.x * B.z + A.y * B.w + A.z * B.x,
                     A.w * B.z + A.x * B.y - A.y * B.x + A.z * B.w, A.w * B.w - A.x * B.x - A.y * B.y - A.z * B.z };
        }
        // the angle between two rotations, in degrees
        float Degrees(const Quat& A, const Quat& B)
        {
            const float D = std::fabs(A.x * B.x + A.y * B.y + A.z * B.z + A.w * B.w);
            return 2.0f * std::acos(std::min(1.0f, D)) * 57.29578f;
        }

        // An SEModel's skeleton (names, then per bone flags, parent, global pos + rotation, local, scale:
        // SEModelExport.cpp), read no further. Body: each bone's bind rotation in its parent's space,
        // derived from the global ones as the preview does (SkeletalPreview.cpp), for the bones of its
        // body (none of the head or the hands, no tag_*, no j_gun); the others are left out. A skeleton
        // with no mesh has nothing to show: no bones.
        bool SEModelSkeleton(const std::string& Path, std::vector<std::string>& Names, std::vector<std::pair<size_t, Quat>>& Body)
        {
            Names.clear();
            Body.clear();
            std::ifstream In(fs::path(Strings::ToUnicodeString(Path)), std::ios::binary);
            std::vector<uint8_t> Bytes(29);
            if (!In.read((char*)Bytes.data(), 29) || std::memcmp(Bytes.data(), "SEModel", 7) != 0) return false;
            uint32_t Count = 0, Meshes = 0;
            std::memcpy(&Count, Bytes.data() + 14, 4);
            std::memcpy(&Meshes, Bytes.data() + 18, 4);
            const uint8_t Data = Bytes[11], BoneFlags = Bytes[12];
            if (!(Data & 2) || Meshes == 0 || !(Data & 1)) return true;
            if (Count > 100000) return false;
            const size_t Each = 5 + ((BoneFlags & 1) ? 28 : 0) + ((BoneFlags & 2) ? 28 : 0) + ((BoneFlags & 4) ? 12 : 0);
            for (size_t Want = 29 + (size_t)Count * (24 + Each) + 64;; Want *= 2)
            {
                const size_t Had = Bytes.size();
                Bytes.resize(Want);
                In.read((char*)Bytes.data() + Had, (std::streamsize)(Want - Had));
                Bytes.resize(Had + (size_t)In.gcount());
                size_t End = 29;
                if (CStrings(Bytes, 29, Count, Names))
                {
                    for (const auto& N : Names) End += N.size() + 1;
                    if (End + (size_t)Count * Each <= Bytes.size()) break;
                }
                if (!In) return false;
            }
            size_t At = 29;
            for (const auto& N : Names) At += N.size() + 1;
            if (!(BoneFlags & 1)) return true;   // no global rotations: names only
            std::vector<int32_t> Parent(Count);
            std::vector<Quat> Global(Count);
            for (uint32_t i = 0; i < Count; i++, At += Each)
            {
                std::memcpy(&Parent[i], Bytes.data() + At + 1, 4);
                float Q[4];
                std::memcpy(Q, Bytes.data() + At + 5 + 12, 16);
                Global[i] = QNorm({ Q[0], Q[1], Q[2], Q[3] });
            }
            // the head and the hands (j_head, j_wrist_le/_ri and what their chains of parents reach: a loop or a
            // parent out of range ends the walk), whose joints vary from model to model (a face, a glove's fingers)
            std::vector<int8_t> Out(Count, -1);
            const std::function<bool(uint32_t, int)> HeadOrHand = [&](uint32_t i, int Depth) -> bool {
                if (Out[i] >= 0) return Out[i] != 0;
                bool H = _stricmp(Names[i].c_str(), "j_head") == 0 || _stricmp(Names[i].c_str(), "j_wrist_le") == 0 || _stricmp(Names[i].c_str(), "j_wrist_ri") == 0;
                if (!H && Depth < 512 && Parent[i] >= 0 && (uint32_t)Parent[i] < Count && (uint32_t)Parent[i] != i) H = HeadOrHand((uint32_t)Parent[i], Depth + 1);
                Out[i] = H ? 1 : 0;
                return H;
            };
            for (uint32_t i = 0; i < Count; i++)
            {
                // a tag, a weapon's joint (j_gun) neither
                if (HeadOrHand(i, 0) || _strnicmp(Names[i].c_str(), "tag_", 4) == 0 || _strnicmp(Names[i].c_str(), "j_gun", 5) == 0) continue;
                const int32_t P = Parent[i];
                const Quat Local = P < 0 || (uint32_t)P >= Count ? Global[i]
                                                                 : QNorm(QMul({ -Global[P].x, -Global[P].y, -Global[P].z, Global[P].w }, Global[i]));
                Body.push_back({ i, Local });
            }
            return true;
        }

        // An anim's rotation at its first frame as the preview shows it (SkeletalPreview.cpp: the first key,
        // one no pose has skipped) for the bones of Want (lower case) that have rotation keys and are not
        // additive (an additive key is a change to the bind pose beneath, whatever it is). SEAnim: the
        // key blocks of the other bones are stepped over, not read.
        void AnimStartRotations(const std::string& Path, const std::unordered_set<std::string>& Want, std::unordered_map<std::string, Quat>& Out)
        {
            Out.clear();
            const auto SaneQ = [](const float* V) {
                for (int i = 0; i < 4; i++) if (!std::isfinite(V[i]) || std::fabs(V[i]) > 1e9f) return false;
                return true;
            };
            std::vector<uint8_t> B;
            if (Lower(fs::path(Path).extension().string()) != ".seanim")
            {
                SeAnimClip Clip;
                if (!ReadAnimClip(Path, Clip)) return;
                for (const auto& T : Clip.Bones)
                {
                    const SeAnimBoneType Ty = T.HasTypeOverride ? T.TypeOverride : Clip.Type;
                    if (Ty == SeAnimBoneType::Additive || T.RotKeys.empty() || !Want.count(Lower(T.Name))) continue;
                    const auto& V = T.RotKeys.front().Value;
                    Out[Lower(T.Name)] = QNorm({ V.x, V.y, V.z, V.w });
                }
                return;
            }
            if (!ReadAll(Path, B) || B.size() < 36 || std::memcmp(B.data(), "SEAnim", 6) != 0) return;
            const uint8_t Type = B[10], Present = B[12], Prop = B[13];
            uint32_t Frames = 0, Count = 0;
            std::memcpy(&Frames, B.data() + 20, 4);
            std::memcpy(&Count, B.data() + 24, 4);
            const uint8_t Modifiers = B[28];
            if (Prop & 1) return;   // double precision: not written by any exporter here, nor read by the preview
            const size_t FrameSz = Frames <= 0xFF ? 1 : Frames <= 0xFFFF ? 2 : 4;
            std::vector<std::string> Names;
            if (Count > 100000 || !CStrings(B, 36, Count, Names)) return;
            size_t At = 36;
            for (const auto& N : Names) At += N.size() + 1;
            std::vector<uint8_t> Ty(Count, Type);
            const size_t ModSz = Count <= 0xFF ? 1 : 2;
            for (uint32_t i = 0; i < Modifiers; i++, At += ModSz + 1)
            {
                if (At + ModSz + 1 > B.size()) return;
                const uint32_t Idx = ModSz == 1 ? B[At] : (uint32_t)(B[At] | (B[At + 1] << 8));
                if (Idx < Count) Ty[Idx] = B[At + ModSz];
            }
            const auto Uint = [&](size_t Pos) -> uint32_t {
                uint32_t V = 0;
                if (Pos + FrameSz <= B.size()) std::memcpy(&V, B.data() + Pos, FrameSz);
                return V;
            };
            size_t Found = 0;
            for (uint32_t i = 0; i < Count && Found < Want.size(); i++)
            {
                At++;   // bone flags
                if (Present & 1) At += FrameSz + (size_t)Uint(At) * (FrameSz + 12);
                if (Present & 2)
                {
                    const uint32_t Keys = Uint(At);
                    At += FrameSz;
                    if (At + (size_t)Keys * (FrameSz + 16) > B.size()) return;
                    if (Ty[i] != (uint8_t)SeAnimBoneType::Additive && Want.count(Lower(Names[i])))
                        for (uint32_t k = 0; k < Keys; k++)
                        {
                            float V[4];
                            std::memcpy(V, B.data() + At + k * (FrameSz + 16) + FrameSz, 16);
                            if (SaneQ(V)) { Out[Lower(Names[i])] = QNorm({ V[0], V[1], V[2], V[3] }); Found++; break; }
                        }
                    At += (size_t)Keys * (FrameSz + 16);
                }
                if (Present & 4) At += FrameSz + (size_t)Uint(At) * (FrameSz + 12);
                if (At > B.size()) return;
            }
        }

        // Of Want's bones (lower case), those the anim moves (a rotation keyed over more than 5 degrees or a
        // translation over half a unit: a tentacle, a cymbal's arm) and those it holds still more than 10
        // degrees from rest (a tongue's pose). Keyed at rest only, or not keyed: neither (BO7's Shade keys
        // its toes and a dozen hashed joints at rest, holds its tongue, moves six joints).
        struct Shapes { size_t Moving = 0, Held = 0; };
        // One bone's keys: Rot(k) its k-th rotation (x y z w), Pos(k) its k-th translation
        template <typename RotFn, typename PosFn>
        void Shape(size_t Rots, RotFn Rot, size_t Poss, PosFn Pos, Shapes& Out)
        {
            bool Moving = false, Held = false;
            if (Rots > 0)
            {
                const Quat First = QNorm(Rot(0));
                Held = Degrees(First, Quat{}) > 10.0f;
                for (size_t k = 1; k < Rots && !Moving; k++) Moving = Degrees(First, QNorm(Rot(k))) > 5.0f;
            }
            if (Poss > 0)
            {
                const auto A = Pos(0);
                for (size_t k = 1; k < Poss && !Moving; k++)
                {
                    const auto B = Pos(k);
                    Moving = (A[0] - B[0]) * (A[0] - B[0]) + (A[1] - B[1]) * (A[1] - B[1]) + (A[2] - B[2]) * (A[2] - B[2]) > 0.25f;
                }
            }
            Out.Moving += Moving;
            Out.Held += !Moving && Held;
        }

        Shapes ShapedBones(const std::string& Path, const std::unordered_set<std::string>& Want)
        {
            Shapes Out;
            if (Want.empty()) return Out;
            std::vector<uint8_t> B;
            if (Lower(fs::path(Path).extension().string()) != ".seanim")
            {
                SeAnimClip Clip;
                if (!ReadAnimClip(Path, Clip)) return Out;
                for (const auto& T : Clip.Bones)
                    if (Want.count(Lower(T.Name)))
                        Shape(T.RotKeys.size(), [&](size_t k) { const auto& V = T.RotKeys[k].Value; return Quat{ V.x, V.y, V.z, V.w }; },
                              T.PosKeys.size(), [&](size_t k) { const auto& V = T.PosKeys[k].Value; return std::array<float, 3>{ V.x, V.y, V.z }; }, Out);
                return Out;
            }
            // SEAnim (AnimStartRotations' layout): the key blocks of the other bones stepped over
            if (!ReadAll(Path, B) || B.size() < 36 || std::memcmp(B.data(), "SEAnim", 6) != 0) return Out;
            const uint8_t Present = B[12], Prop = B[13];
            uint32_t Frames = 0, Count = 0;
            std::memcpy(&Frames, B.data() + 20, 4);
            std::memcpy(&Count, B.data() + 24, 4);
            if (Prop & 1) return Out;
            const size_t FrameSz = Frames <= 0xFF ? 1 : Frames <= 0xFFFF ? 2 : 4;
            std::vector<std::string> Names;
            if (Count > 100000 || !CStrings(B, 36, Count, Names)) return Out;
            size_t At = 36;
            for (const auto& N : Names) At += N.size() + 1;
            At += (size_t)B[28] * ((Count <= 0xFF ? 1 : 2) + 1);
            const auto Uint = [&](size_t P) -> size_t {
                uint32_t V = 0;
                if (P + FrameSz <= B.size()) std::memcpy(&V, B.data() + P, FrameSz);
                return V;
            };
            const auto Floats = [&](size_t P, float* V, int N) { std::memcpy(V, B.data() + P, sizeof(float) * N); };
            for (uint32_t i = 0; i < Count; i++)
            {
                At++;   // bone flags
                size_t PosAt = 0, PosN = 0, RotAt = 0, RotN = 0;
                if (Present & 1) { PosN = Uint(At); PosAt = At + FrameSz; At = PosAt + PosN * (FrameSz + 12); }
                if (Present & 2) { RotN = Uint(At); RotAt = At + FrameSz; At = RotAt + RotN * (FrameSz + 16); }
                if (Present & 4) At += FrameSz + Uint(At) * (FrameSz + 12);
                if (At > B.size()) return Out;
                if (!Want.count(Lower(Names[i]))) continue;
                Shape(RotN, [&](size_t k) { float V[4]; Floats(RotAt + k * (FrameSz + 16) + FrameSz, V, 4); return Quat{ V[0], V[1], V[2], V[3] }; },
                      PosN, [&](size_t k) { std::array<float, 3> V; Floats(PosAt + k * (FrameSz + 12) + FrameSz, V.data(), 3); return V; }, Out);
            }
            return Out;
        }

        const CastFile::Node* FindNode(const std::vector<CastFile::Node>& Nodes, uint32_t Id)
        {
            for (const auto& N : Nodes)
            {
                if (N.Id == Id) return &N;
                if (const CastFile::Node* F = FindNode(N.Children, Id)) return F;
            }
            return nullptr;
        }
    }

    bool ModelBoneNames(const std::string& Path, std::vector<std::string>& Out)
    {
        Out.clear();
        const std::string Ext = Lower(fs::path(Path).extension().string());
        if (Ext == ".semodel")   // "SEModel" ver hdrSize dataFlags@11 boneFlags meshFlags boneCount@14 meshCount@18 ... names@29
        {
            // a skeleton with no mesh has nothing to show: no rig
            if (MeshlessSEModel(fs::path(Strings::ToUnicodeString(Path)))) return true;
            return ReadNames(Path, 29, 14, 29, 11, 1, "SEModel", Out);
        }
        if (Ext == ".xmodel_export")
        {
            std::ifstream In(fs::path(Strings::ToUnicodeString(Path)));
            std::string Line;
            int Count = -1;
            while (std::getline(In, Line))
            {
                if (Count < 0 && Line.rfind("NUMBONES ", 0) == 0)
                {
                    Count = std::atoi(Line.c_str() + 9);
                    if (Count == 0) return true;
                }
                else if (Count > 0 && Line.rfind("BONE ", 0) == 0)
                {
                    const auto A = Line.find('"'), B = Line.rfind('"');
                    if (A != std::string::npos && B > A) Out.push_back(Line.substr(A + 1, B - A - 1));
                    if ((int)Out.size() == Count) return true;
                }
            }
            return Count >= 0;
        }
        if (Ext == ".cast")
        {
            std::vector<uint8_t> Bytes;
            std::vector<CastFile::Node> Roots;
            if (!ReadAll(Path, Bytes) || !CastFile::Parse(Bytes, Roots)) return false;
            const CastFile::Node* Model = FindNode(Roots, CastFile::IdModel);
            if (!Model) return false;
            for (const auto& C : Model->Children)
                if (C.Id == CastFile::IdSkeleton)
                    for (const auto& B : C.Children)
                        if (B.Id == CastFile::IdBone) Out.push_back(B.String("n"));
            return true;
        }
        return false;
    }

    bool AnimBoneNames(const std::string& Path, std::vector<std::string>& Out)
    {
        Out.clear();
        const std::string Ext = Lower(fs::path(Path).extension().string());
        if (Ext == ".seanim")   // "SEAnim" ver hdrSize type flags dataFlags dataProp reserved rate frames boneCount@24 ... names@36
            return ReadNames(Path, 36, 24, 36, 0, 0, "SEAnim", Out);
        SeAnimClip Clip;
        if (!ReadAnimClip(Path, Clip)) return false;
        for (const auto& B : Clip.Bones) Out.push_back(B.Name);
        return true;
    }

    std::string GameOf(const std::string& Path)
    {
        static const std::set<std::string> Kinds = [] {
            std::set<std::string> K;
            for (const auto& Info : DiskGame::Kinds) K.insert(Info.Folder);
            K.insert("xanims");
            K.insert("xmodels");
            return K;
        }();
        std::vector<std::string> Parts;
        for (const auto& P : fs::path(Path).parent_path()) Parts.push_back(Lower(P.string()));
        for (size_t i = Parts.size(); i-- > 1;)
            if (Kinds.count(Parts[i])) return Parts[i - 1];
        return {};
    }

    struct Index::Impl
    {
        // a model as the picks see it
        struct Model
        {
            std::string Path, Name, Game, Base;   // Base: its name without _LODn
            Role R = Role::Other;
            std::set<std::string> Words;
            std::vector<int> Bones;               // sorted ids
            bool Reference = false;
            bool RefBody = false, RefHead = false;   // a game's reference body, head (the table's lists)
            std::string Kind;                        // what it is ("zombie", "dog"; "" a player): its name, or its reference's
            int Skel = 0;                            // the same for models of the same bones
            int Family = 0;                          // the same for models of the same game and kind
            const std::vector<std::pair<int, Quat>>* Pose = nullptr;   // its body's bind rotations by bone id (sorted), when its format has them
        };
        struct Result
        {
            Pick P;
            std::vector<Candidate> Ranked;
            uint64_t Key = 0;
        };
        // a game's reference models
        struct Refs
        {
            enum class State { None, Queued, Done } S = State::None;
            std::vector<ModelRef> Models;
            std::string Note;
        };

        mutable std::mutex M;
        std::condition_variable Cv;
        // under M
        std::vector<ModelRef> Listed;
        uint64_t ListedVersion = 0;
        bool HaveModels = false;
        uint64_t RefVersion = 0;
        std::deque<std::string> Queue;
        std::unordered_set<std::string> Queued;
        std::unordered_map<std::string, Result> Results;
        std::unordered_map<std::string, std::string> Manual;
        std::map<std::string, Refs> GameRefs;
        std::deque<std::string> RefQueue;
        bool Fetch = true, Stop = false, Busy = false, RefBusy = false;
        std::atomic<bool> Stopping{ false };   // Stop, read without M: an export under way is ended
        std::atomic<uint64_t> Gen{ 1 };
        std::atomic<size_t> PoseDecided{ 0 };   // picks the bind pose made (Stats)
        size_t PoolSize = 0;
        // the table, read once (under M): per game its lists of names (hands, body, head), the first found of each used
        using Lists = std::vector<std::vector<std::string>>;
        std::map<std::string, Lists> Table;
        std::map<std::string, std::map<std::string, std::string>> Bodies, Heads;   // per game, the table's body and head names (lower case) -> their kind
        std::set<std::string> FromInstall;   // games whose references come from the install folder whole ("from": "install")
        // the kinds an anim or a model can be ("kinds", in order: the first a name fits), and the names none is
        struct KindNames { std::string Kind; std::vector<std::string> Anims, Models; };
        std::vector<KindNames> Kinds;
        std::vector<std::string> NoKind;
        std::vector<std::string> OptionalJoints;   // the joints a model may lack (the table's "optional_joints": a tag, a coat's)
        bool TableRead = false;

        // Name's kind ("" when none): an anim's (Anim) or a model's
        std::string KindOf(const std::string& Name, bool Anim) const
        {
            const std::string N = Lower(Name);
            if (AnyGlob(N, NoKind)) return {};
            for (const auto& K : Kinds)
                if (AnyGlob(N, Anim ? K.Anims : K.Models)) return K.Kind;
            return {};
        }

        // the worker's own
        std::unordered_map<std::string, int> BoneIds;
        std::unordered_map<std::string, std::vector<int>> BonesOf;   // by path
        std::unordered_map<std::string, std::vector<std::pair<int, Quat>>> PoseOf;   // by path: its body's bind rotations (an SEModel's)
        std::set<std::string> FoldersLoaded;
        std::map<std::string, std::vector<std::string>> FolderDirty;  // folder -> paths read since its cache was written
        std::vector<Model> Pool;
        uint64_t PoolKey = ~0ull;
        // the bones some model of the pool has, per game and of any game (by id): what a score counts
        std::map<std::string, std::vector<uint8_t>> Known;
        std::vector<uint8_t> KnownAll;
        // per bone id, the models of the pool holding it (their places in Pool)
        std::vector<std::vector<int>> Holders;
        // Choose's buffers, kept from one anim to the next
        std::vector<int> Slot, GroupOf, GroupTotal, GroupOrder;
        std::vector<std::vector<int>> Members;   // per skeleton of the anim's candidates, their places in All
        std::vector<uint64_t> GroupBits;
        std::vector<uint8_t> Mark, RigMark;
        std::vector<int> Shared;   // per model of the pool, the joints it shares with the pick (-1: not counted yet)
        size_t SkelCount = 0;

        std::thread Worker, RefWorker;

        // the games' anim owners (anim_owners.json), the worker's own: per file, read again when it changes;
        // each owner id held once (CoD Online's file names up to 22 owners per anim, 3.5 MB)
        struct OwnerFile
        {
            std::string Stamp;
            std::vector<std::pair<std::string, AnimOwners::Owner>> Owners;   // id, owner
            std::unordered_map<std::string, std::vector<uint32_t>> Anims;    // lower-case anim -> its owners' places in Owners
        };
        std::map<std::string, OwnerFile> OwnerFiles;
        std::unordered_map<std::string, bool> MeshlessOf;   // by path: a browser's model with no mesh (no owner's part)
        std::unordered_map<std::string, const Model*> PoolByName;   // "<game>|<lower-case base>", the pool's first
        uint64_t PoolByNameKey = ~0ull;
        // under M: owners' models to export from the install, (game, names), and those asked this session
        std::deque<std::pair<std::string, std::vector<std::string>>> OwnerQueue;
        std::set<std::string> OwnerAsked, OwnerPending;   // "<game>|<lower-case first name>"; pending: queued or fetching

        uint64_t KeyNow() const { return (ListedVersion << 20) ^ RefVersion; }

        int BoneId(const std::string& Name)
        {
            const auto [It, New] = BoneIds.emplace(Lower(Name), (int)BoneIds.size());
            return It->second;
        }

        // the bone-name cache a model's names are kept in: its game folder's (above models\), else its folder's
        static std::string CacheFolder(const std::string& Path)
        {
            std::vector<fs::path> Up;
            for (fs::path D = fs::path(Path).parent_path(); !D.empty() && D != D.parent_path(); D = D.parent_path()) Up.push_back(D);
            for (size_t i = 0; i + 1 < Up.size(); i++)
                if (Lower(Up[i].filename().string()) == "models") return Up[i + 1].string();
            return fs::path(Path).parent_path().string();
        }
        static fs::path CacheFile(const std::string& Folder)
        {
            uint64_t H = 1469598103934665603ull;
            for (const char c : Lower(Folder)) { H ^= (uint8_t)c; H *= 1099511628211ull; }
            char Name[40];
            // .2: a model with no mesh has no bones; .3: an SEModel's body bind rotations (a 4th column)
            snprintf(Name, sizeof(Name), "%016llx.3.tsv", (unsigned long long)H);
            return RigsRoot() / "bones" / Name;
        }
        static std::string Stamp(const std::string& Path)
        {
            std::error_code Ec, Ec2;
            const fs::path P(Strings::ToUnicodeString(Path));
            const auto Size = fs::file_size(P, Ec);
            const auto When = fs::last_write_time(P, Ec2).time_since_epoch().count();
            return std::to_string(Ec ? 0 : Size) + ":" + std::to_string(Ec2 ? 0 : When);
        }

        void LoadFolderCache(const std::string& Folder)
        {
            if (!FoldersLoaded.insert(Folder).second) return;
            std::ifstream In(CacheFile(Folder));
            std::string Line;
            while (std::getline(In, Line))
            {
                std::stringstream Ss(Line);
                std::string Path, St, Names, Rots;
                if (!std::getline(Ss, Path, '\t') || !std::getline(Ss, St, '\t')) continue;
                std::getline(Ss, Names, '\t');
                const bool HasPose = (bool)std::getline(Ss, Rots);
                if (Stamp(Path) != St) continue;
                std::vector<int> Ids;
                std::stringstream Ns(Names), Rs(Rots);
                std::string B, R;
                std::vector<std::pair<int, Quat>> Pose;
                while (std::getline(Ns, B, ','))
                {
                    const bool Rot = HasPose && std::getline(Rs, R, ',') && !R.empty();
                    if (B.empty()) continue;
                    Ids.push_back(BoneId(B));
                    Quat Q;
                    if (Rot && sscanf_s(R.c_str(), "%f %f %f %f", &Q.x, &Q.y, &Q.z, &Q.w) == 4) Pose.push_back({ Ids.back(), Q });
                }
                std::sort(Ids.begin(), Ids.end());
                Ids.erase(std::unique(Ids.begin(), Ids.end()), Ids.end());
                BonesOf[Path] = std::move(Ids);
                if (HasPose) PoseOf[Path] = SortedPose(std::move(Pose));
            }
        }

        // by bone id, the first of a name given twice kept
        static std::vector<std::pair<int, Quat>> SortedPose(std::vector<std::pair<int, Quat>> Pose)
        {
            std::stable_sort(Pose.begin(), Pose.end(), [](const auto& A, const auto& B) { return A.first < B.first; });
            Pose.erase(std::unique(Pose.begin(), Pose.end(), [](const auto& A, const auto& B) { return A.first == B.first; }), Pose.end());
            return Pose;
        }

        void SaveFolderCaches()
        {
            std::vector<std::string> Names(BoneIds.size());
            for (const auto& [N, Id] : BoneIds) Names[Id] = N;
            for (auto& [Folder, Paths] : FolderDirty)
            {
                if (Paths.empty()) continue;
                std::error_code Ec;
                const fs::path F = CacheFile(Folder);
                fs::create_directories(F.parent_path(), Ec);
                std::ofstream Out(F, std::ios::app);
                for (const auto& Path : Paths)
                {
                    Out << Path << '\t' << Stamp(Path) << '\t';
                    const auto It = BonesOf.find(Path);
                    if (It != BonesOf.end())
                        for (size_t i = 0; i < It->second.size(); i++) Out << (i ? "," : "") << Names[It->second[i]];
                    // the bind rotations, in the names' order (none for a bone out of the body)
                    if (const auto Po = PoseOf.find(Path); Po != PoseOf.end() && It != BonesOf.end())
                    {
                        Out << '\t';
                        for (size_t i = 0; i < It->second.size(); i++)
                        {
                            if (i) Out << ',';
                            const auto Q = std::lower_bound(Po->second.begin(), Po->second.end(), It->second[i],
                                                            [](const auto& P, int Id) { return P.first < Id; });
                            if (Q == Po->second.end() || Q->first != It->second[i]) continue;
                            char Buf[80];
                            snprintf(Buf, sizeof(Buf), "%.6g %.6g %.6g %.6g", Q->second.x, Q->second.y, Q->second.z, Q->second.w);
                            Out << Buf;
                        }
                    }
                    Out << '\n';
                }
                Paths.clear();
            }
        }

        const std::vector<int>& BonesFor(const std::string& Path)
        {
            if (const auto It = BonesOf.find(Path); It != BonesOf.end()) return It->second;
            const std::string Folder = CacheFolder(Path);
            LoadFolderCache(Folder);
            if (const auto It = BonesOf.find(Path); It != BonesOf.end()) return It->second;
            std::vector<std::string> Names;
            std::vector<int> Ids;
            std::vector<std::pair<size_t, Quat>> Body;
            if (Lower(fs::path(Path).extension().string()) == ".semodel")
            {
                if (SEModelSkeleton(Path, Names, Body))
                {
                    std::vector<std::pair<int, Quat>> Pose;
                    for (const auto& [i, Q] : Body) Pose.push_back({ BoneId(Names[i]), Q });
                    PoseOf[Path] = SortedPose(std::move(Pose));
                }
                else Names.clear();
            }
            else if (!ModelBoneNames(Path, Names)) Names.clear();
            for (const auto& N : Names) Ids.push_back(BoneId(N));
            std::sort(Ids.begin(), Ids.end());
            Ids.erase(std::unique(Ids.begin(), Ids.end()), Ids.end());
            FolderDirty[Folder].push_back(Path);
            return BonesOf[Path] = std::move(Ids);
        }

        // the models picks choose from: one per name and folder (its lowest LOD number), the games' references
        void BuildPool(const std::vector<ModelRef>& Models, const std::map<std::string, Refs>& References)
        {
            std::map<std::string, std::pair<int, const ModelRef*>> ByName;   // folder|base -> (lod, model)
            for (const auto& L : Models)
            {
                int Lod = 0;
                const std::string Base = WithoutLod(L.Name, &Lod);
                const std::string Key = Lower(fs::path(L.Path).parent_path().string() + "|" + Base);
                const auto It = ByName.find(Key);
                if (It == ByName.end() || Lod < It->second.first) ByName[Key] = { Lod, &L };
            }
            Pool.clear();
            std::map<std::vector<int>, int> Skels;
            std::map<std::string, int> Families;
            const auto Add = [&](const ModelRef& L, bool Reference) {
                // the game's tag_origin model is a debug plane things are attached to, not something an anim moves
                if (_stricmp(WithoutLod(L.Name).c_str(), "tag_origin") == 0) return;
                Model Md;
                Md.Path = L.Path; Md.Name = L.Name; Md.Base = WithoutLod(L.Name); Md.Game = GameOf(L.Path);
                Md.R = ModelRole(Md.Base); Md.Words = Words(Md.Base); Md.Reference = Reference;
                Md.Kind = KindOf(Md.Base, false);
                for (const auto* Refs : { &Bodies, &Heads })
                {
                    const auto G = Refs->find(Md.Game);
                    if (!Reference || G == Refs->end()) continue;
                    if (const auto It = G->second.find(Lower(Md.Base)); It != G->second.end())
                    {
                        (Refs == &Bodies ? Md.RefBody : Md.RefHead) = true;
                        Md.Kind = It->second;
                    }
                }
                Md.Bones = BonesFor(L.Path);
                if (const auto Po = PoseOf.find(L.Path); Po != PoseOf.end() && !Po->second.empty()) Md.Pose = &Po->second;
                Md.Skel = Skels.emplace(Md.Bones, (int)Skels.size()).first->second;
                Md.Family = Families.emplace(Md.Game + "|" + Md.Kind, (int)Families.size()).first->second;
                if (!Md.Bones.empty()) Pool.push_back(std::move(Md));
            };
            for (const auto& [Key, V] : ByName) Add(*V.second, false);
            for (const auto& [Game, R] : References)
                for (const auto& L : R.Models) Add(L, true);
            SkelCount = Skels.size();
            Holders.assign(BoneIds.size(), {});
            for (size_t m = 0; m < Pool.size(); m++)
                for (const int B : Pool[m].Bones) Holders[B].push_back((int)m);
            Known.clear();
            KnownAll.assign(BoneIds.size(), 0);
            for (const Model& Md : Pool)
            {
                // a kind's reference counts for the anims of its kind only ("<game>|<kind>")
                auto& K = Known[Md.Reference && !Md.Kind.empty() ? Md.Game + "|" + Md.Kind : Md.Game];
                K.resize(BoneIds.size(), 0);
                for (const int B : Md.Bones) K[B] = KnownAll[B] = 1;
            }
            for (auto& [Key, K] : Known)
                if (const auto Bar = Key.find('|'); Bar != std::string::npos)
                    if (const auto G = Known.find(Key.substr(0, Bar)); G != Known.end())
                        for (size_t i = 0; i < K.size() && i < G->second.size(); i++) K[i] |= G->second[i];
            SaveFolderCaches();
        }

        struct Scored
        {
            const Model* M = nullptr;
            int Hits = 0, Extra = 0, Words = 0;
            bool SameGame = false, RoleFits = false, KindFits = false, Joined = false;
            float Score = 0;
        };

        // Better first: the score (in steps of 2%; a rig joining a second model to its first pays the 4% that
        // model has to add, a one-piece body staying before a body and a head fitting as well), then the
        // game, the kind, the words, fewer bones left still
        static int Step(const Scored& S) { return (int)((S.Score - (S.Joined ? 0.04f : 0.0f)) * 50.0f + 1e-4f + 50.0f); }
        // (Exact, among rigs: of one step and the same names, a model alone first, then the pair holding more
        // of the anim's bones: a pair's step moves with the 4% it pays, hands and their weapon took a
        // placeholder for the hands)
        static bool Before(const Scored& A, const Scored& B, bool Exact = false)
        {
            const int Sa = Step(A), Sb = Step(B);
            if (Sa != Sb) return Sa > Sb;
            if (A.SameGame != B.SameGame) return A.SameGame;
            if (A.RoleFits != B.RoleFits) return A.RoleFits;
            if (A.Words != B.Words) return A.Words > B.Words;
            const float Ka = A.Joined ? A.Score : 2.0f, Kb = B.Joined ? B.Score : 2.0f;
            if (Exact && Ka != Kb) return Ka > Kb;
            if (A.Extra != B.Extra) return A.Extra < B.Extra;
            return A.M->Path < B.M->Path;
        }

        // A model with what completes it: the rig it makes for an anim, scored whole (a body and its
        // separate head beside a one-piece model)
        struct Rig
        {
            Scored S;                       // S.M the model; S.Score, S.Joined the whole rig's
            const Model* First = nullptr;   // the skeleton the others join
            const Model* Other = nullptr;   // the model holding most of the anim's bones the first lacks
            const Model* Head = nullptr;    // a reference body's head
            int Gain = 0, HeadGain = 0;     // the anim's bones Other, Head add
            bool Whole = false;             // completed (else S is the model alone)
        };

        // A rig's body bind rotation of bone Id: its first model's, else the joined one's, else its head's
        static const Quat* RigBind(const Rig& Rg, int Id)
        {
            for (const Model* Md : { Rg.First, Rg.Other, Rg.Head })
                if (Md && Md->Pose)
                {
                    const auto It = std::lower_bound(Md->Pose->begin(), Md->Pose->end(), Id, [](const auto& P, int I) { return P.first < I; });
                    if (It != Md->Pose->end() && It->first == Id) return &It->second;
                }
            return nullptr;
        }

        // Of Band (the rigs fitting an anim as well as the best, in their order), the first of the bind the
        // anim's first frame is nearest, when the rigs of any other bind are farther by more than 30 degrees
        // (rigmeasure, 2026-10-26: BO2 and BO3 zombie and player anims, none wrong of 170 so, 12 left to
        // the names): the mean angle, over the body bones whose binds differ by more than 30 degrees between
        // two rigs of the band and that the anim keys, between its first rotation and the rig's bind. Kept:
        // the band's rigs of that bind. Null: nothing told (one bind, no such bone keyed, a margin of 30 or
        // less, a model whose format gives no bind rotations).
        const Rig* PoseChoice(const std::string& AnimPath, const std::vector<std::string>& Names, const std::vector<const Rig*>& Band,
                              std::unordered_set<const Rig*>& Kept)
        {
            Kept.clear();
            if (Band.size() < 2) return nullptr;
            for (const Rig* Rg : Band)
                if (Rg->First->Pose == nullptr) return nullptr;
            // more than 30 degrees apart: |dot| under cos(15 degrees)
            const auto Far = [](const Quat& A, const Quat& B) { return std::fabs(A.x * B.x + A.y * B.y + A.z * B.z + A.w * B.w) < 0.96592583f; };
            std::vector<int> Ids;
            std::vector<std::string> IdNames;
            std::unordered_set<int> Seen;
            std::vector<const Quat*> Q(Band.size());
            for (const auto& Nm : Names)
            {
                const int Id = BoneId(Nm);
                if (!Seen.insert(Id).second) continue;
                bool Every = true, Differ = false;
                for (size_t r = 0; r < Band.size() && Every; r++) Every = (Q[r] = RigBind(*Band[r], Id)) != nullptr;
                for (size_t i = 0; Every && i < Q.size() && !Differ; i++)
                    for (size_t j = i + 1; j < Q.size() && !Differ; j++) Differ = Far(*Q[i], *Q[j]);
                if (Every && Differ) { Ids.push_back(Id); IdNames.push_back(Lower(Nm)); }
            }
            if (Ids.empty()) return nullptr;
            std::unordered_map<std::string, Quat> Start;
            AnimStartRotations(AnimPath, std::unordered_set<std::string>(IdNames.begin(), IdNames.end()), Start);
            std::vector<int> Used;
            std::vector<float> Mean(Band.size(), 0.0f);
            for (size_t k = 0; k < Ids.size(); k++)
                if (const auto It = Start.find(IdNames[k]); It != Start.end())
                {
                    Used.push_back(Ids[k]);
                    for (size_t r = 0; r < Band.size(); r++) Mean[r] += Degrees(It->second, *RigBind(*Band[r], Ids[k]));
                }
            if (Used.empty()) return nullptr;
            size_t Win = 0;
            for (size_t r = 0; r < Band.size(); r++)
            {
                Mean[r] /= (float)Used.size();
                if (Mean[r] < Mean[Win] - 1e-3f) Win = r;
            }
            // the rigs of another bind: one of those bones more than 30 degrees from the nearest rig's
            float Margin = 1e9f;
            std::vector<uint8_t> Other(Band.size(), 0);
            for (size_t r = 0; r < Band.size(); r++)
            {
                for (const int Id : Used)
                    if (Far(*RigBind(*Band[r], Id), *RigBind(*Band[Win], Id))) { Other[r] = 1; break; }
                if (Other[r]) Margin = std::min(Margin, Mean[r] - Mean[Win]);
            }
            if (Margin > 1e8f || Margin <= 30.0f) return nullptr;
            const Rig* Chosen = nullptr;
            for (size_t r = 0; r < Band.size(); r++)
                if (!Other[r])
                {
                    Kept.insert(Band[r]);
                    if (Chosen == nullptr) Chosen = Band[r];
                }
            PoseDecided++;
            return Chosen;
        }

        // the share of an anim's bones no model has, shaped, from which the model it was made for is not here
        // (anim-rigs 2026-10-26, every game: WaW's sleeve reshapers on its viewhands 11% before they were
        // optional, BO7's Shade on the generic zombie 15%, Neptune 20%, Cold War's dogs on a body 57%)
        static constexpr float OwnModelAbsentShare = 0.125f;

        Result Choose(const std::string& AnimPath, bool& WantRefs)
        {
            Result R;
            WantRefs = false;
            std::vector<std::string> Names;
            if (!AnimBoneNames(AnimPath, Names)) return R;
            const std::string AnimName = fs::path(AnimPath).stem().string();
            const std::string Game = GameOf(AnimPath);
            const std::string Kind = KindOf(AnimName, true);
            // the anim's bones some model of its game has (of any game when the pool holds none of
            // its game; its kind's references counted): a bone no model has (a sleeve's helper joints)
            // costs no rig its score
            auto KnownIt = Known.find(Game + "|" + Kind);
            if (Kind.empty() || KnownIt == Known.end()) KnownIt = Known.find(Game);
            const bool GameKnown = !Game.empty() && KnownIt != Known.end();
            const std::vector<uint8_t>& Counted = GameKnown ? KnownIt->second : KnownAll;
            // tag_origin, which nearly every model has, tells nothing of which one an anim was made for
            // (an ac130 flare anim moving only it played on a debug plane); Own: the anim's bones left,
            // known or not
            std::vector<uint8_t> InAnim;
            std::vector<int> AnimIds;   // the bones counted, once each
            std::map<int, std::string> Unmodelled;   // the bones no model of its game has, but the optional ones
            int N = 0, Own = 0;
            for (const auto& Nm : Names)
            {
                const int Id = BoneId(Nm);
                if (_stricmp(Nm.c_str(), "tag_origin") == 0) continue;
                Own++;
                if ((size_t)Id >= Counted.size() || !Counted[Id])
                {
                    if (std::string L = Lower(Nm); !AnyGlob(L, OptionalJoints)) Unmodelled.emplace(Id, std::move(L));
                    continue;
                }
                if ((size_t)Id >= InAnim.size()) InAnim.resize(Id + 1, 0);
                if (!InAnim[Id]) { InAnim[Id] = 1; N++; AnimIds.push_back(Id); }
            }
            if (N == 0)
            {
                WantRefs = true;
                return R;
            }
            const Role Want = AnimRole(AnimName);
            const std::set<std::string> AnimWords = Words(AnimName);

            std::vector<Scored> All;
            for (const Model& Md : Pool)
            {
                // a kind's reference (a zombie) is for anims of its kind only: a soldier's idle took the
                // zombie having fewer bones left
                if (Md.Reference && !Md.Kind.empty() && Md.Kind != Kind) continue;
                Scored S;
                S.M = &Md;
                for (const int B : Md.Bones) S.Hits += (size_t)B < InAnim.size() && InAnim[B];
                if (S.Hits == 0) continue;
                S.Extra = (int)Md.Bones.size() - S.Hits;
                S.Score = (float)S.Hits / (float)N;
                S.SameGame = !Game.empty() && Md.Game == Game;
                S.RoleFits = Want != Role::Other && Md.R == Want;
                S.KindFits = !Kind.empty() && Md.Kind == Kind;
                for (const auto& W : Md.Words) S.Words += (int)AnimWords.count(W);
                All.push_back(S);
            }
            if (All.empty())
            {
                WantRefs = true;
                return R;
            }
            std::sort(All.begin(), All.end(), [](const Scored& A, const Scored& B) { return Before(A, B); });
            // the user's list, each model alone: the anim's own game first (when one fits half as well
            // as the best of any game's)
            {
                bool Own1 = false;
                for (const auto& S : All) Own1 |= S.SameGame && S.Score >= 0.2f && S.Score >= 0.5f * All.front().Score;
                for (const int Pass : { 0, 1 })
                    for (const auto& S : All)
                        if (R.Ranked.size() < 400 && (Pass == 0) == (S.SameGame || !Own1))
                            R.Ranked.push_back({ S.M->Path, S.M->Base, S.Score, S.M->Reference });
            }

            // a reference body wears its game's reference head of its kind, whatever bones the anim moves
            // (a body the table gives a head for has none of its own)
            const auto RefHeadOf = [&](const Model* Body) -> const Model* {
                if (Body == nullptr || !Body->RefBody) return nullptr;
                for (const Model& Md : Pool)
                    if (Md.RefHead && Md.Game == Body->Game && Md.Kind == Body->Kind) return &Md;
                return nullptr;
            };
            // the anim's bones each skeleton of All holds (Slot: a bone's place in AnimIds), as bits; its
            // models in All's order, the best first
            Slot.assign(InAnim.size(), -1);
            for (size_t k = 0; k < AnimIds.size(); k++) Slot[AnimIds[k]] = (int)k;
            const size_t Words64 = (AnimIds.size() + 63) / 64;
            GroupOf.assign(SkelCount, -1);
            GroupBits.clear();
            if (Members.size() < All.size()) Members.resize(All.size());
            size_t Groups = 0;
            for (size_t j = 0; j < All.size(); j++)
            {
                int& G = GroupOf[All[j].M->Skel];
                if (G < 0)
                {
                    G = (int)Groups++;
                    Members[G].clear();
                    GroupBits.resize(Groups * Words64, 0);
                    for (const int B : All[j].M->Bones)
                        if ((size_t)B < Slot.size() && Slot[B] >= 0) GroupBits[G * Words64 + Slot[B] / 64] |= 1ull << (Slot[B] % 64);
                }
                Members[G].push_back((int)j);
            }
            // most of the anim's bones first: a skeleton holding fewer than the best found adds fewer
            GroupTotal.assign(Groups, 0);
            GroupOrder.resize(Groups);
            for (size_t g = 0; g < Groups; g++)
            {
                GroupOrder[g] = (int)g;
                for (size_t w = 0; w < Words64; w++) GroupTotal[g] += (int)std::popcount(GroupBits[g * Words64 + w]);
            }
            std::sort(GroupOrder.begin(), GroupOrder.begin() + Groups, [&](int A, int B) { return GroupTotal[A] > GroupTotal[B]; });
            Mark.assign(InAnim.size(), 0);
            const auto MarkBones = [&](std::initializer_list<const Model*> Of, uint8_t V) {
                for (const Model* Had : Of)
                    if (Had) for (const int B : Had->Bones) if ((size_t)B < Mark.size()) Mark[B] = V;
            };
            std::vector<uint64_t> Have(Words64);
            // the model holding most of the anim's bones Base (and the head it wears) lacks, of its game:
            // short of bones, a body takes its head, hands their weapon
            const auto Complement = [&](const Model* Base, const Model* BaseHead, int& Gain) -> const Model* {
                Gain = 0;
                std::fill(Have.begin(), Have.end(), 0);
                for (const Model* Had : { Base, BaseHead })
                    if (Had) for (const int B : Had->Bones)
                        if ((size_t)B < Slot.size() && Slot[B] >= 0) Have[Slot[B] / 64] |= 1ull << (Slot[B] % 64);
                int Had = 0;
                for (const uint64_t W : Have) Had += (int)std::popcount(W);
                const Scored* Second = nullptr;
                Scored Top;
                bool Marked = false;
                for (size_t o = 0; o < Groups; o++)
                {
                    const size_t g = (size_t)GroupOrder[o];
                    if (GroupTotal[g] * 25 < N) break;   // less than 4%
                    if (Second != nullptr && Step(Scored{ .Score = (float)GroupTotal[g] / (float)N }) < Step(Top)) break;
                    const uint64_t* Bits = &GroupBits[g * Words64];
                    int G = 0, Shared = 0;
                    for (size_t w = 0; w < Words64; w++)
                    {
                        G += (int)std::popcount(Bits[w] & ~Have[w]);
                        Shared += (int)std::popcount(Bits[w] & Have[w]);
                    }
                    // a second model joins when it adds 4% of the anim's bones (none would add more)
                    if ((float)G / (float)N < 0.04f) continue;
                    // what the base lacks, not another whole: a model holding a third of the anim's bones the
                    // base has overlaps it (a second body, a body over viewhands, CoD Online's defaultactor
                    // over a body), whose rig is its own (a head repeats a dozen of a body's joints, its neck
                    // and shoulders, and adds a few)
                    if (3 * Shared > Had) continue;
                    for (const int j : Members[g])
                    {
                        const Scored& S = All[j];
                        Scored C = S;
                        C.Hits = G;
                        C.Score = (float)G / (float)N;
                        // the skeleton's models are in the order Before gives them: none after one losing
                        if (Second != nullptr && !Before(C, Top)) break;
                        if (S.M == Base || S.M->Base == Base->Base || S.M == BaseHead || S.M->Game != Base->Game) continue;
                        // viewhands and a body share their arms' joint names, not their poses: joined, a
                        // viewmodel anim tore the body apart (MW3's viewmodel_backpack_idle). An anim its
                        // name says is one or the other takes no model of the other kind, nor are the two joined.
                        if ((Want == Role::View && S.M->R == Role::Body) || (Want == Role::Body && S.M->R == Role::View)) continue;
                        if ((Base->R == Role::View && S.M->R == Role::Body) || (Base->R == Role::Body && S.M->R == Role::View)) continue;
                        // no part of another kind's skeleton (a player's head on a zombie body); a weapon,
                        // sharing none of its joints, still joins
                        if (S.M->Kind != Base->Kind)
                        {
                            int Common = 0;
                            for (const int B : S.M->Bones) Common += std::binary_search(Base->Bones.begin(), Base->Bones.end(), B);
                            if (Common >= 2) continue;
                        }
                        // a head is no base for a reference body, which wears its own (two heads)
                        if (const Model* Its = RefHeadOf(S.M))
                        {
                            if (!Marked) { MarkBones({ Base, BaseHead }, 1); Marked = true; }
                            int Common = 0;
                            for (const int B : Its->Bones) Common += (size_t)B < Mark.size() && Mark[B] && InAnim[B];
                            if (Common >= 2) continue;
                        }
                        Second = &S;
                        Top = C;
                        Gain = G;
                        break;
                    }
                }
                if (Marked) MarkBones({ Base, BaseHead }, 0);
                return Second ? Second->M : nullptr;
            };
            // a base's rig: the skeleton the other joins is a body (a head is bound in its own space), else
            // hands, else the one with more bones (hands before their weapon)
            const auto Rank = [](const Model* Md) { return std::make_pair(Md->R == Role::Body ? 2 : Md->R == Role::View ? 1 : 0, Md->Bones.size()); };
            const auto Assemble = [&](const Scored& Base, const Model* Second, int Gain) {
                Rig Rg;
                Rg.S = Base;
                Rg.Whole = true;
                Rg.First = Base.M;
                Rg.Other = Second;
                Rg.Gain = Gain;
                if (Rg.Other && Rank(Rg.Other) > Rank(Rg.First)) std::swap(Rg.First, Rg.Other);
                Rg.Head = Rg.First->RefHead || (Rg.Other && Rg.Other->RefHead) ? nullptr : Rg.First->RefBody ? RefHeadOf(Rg.First) : RefHeadOf(Rg.Other);
                if (Rg.Head)
                {
                    MarkBones({ Rg.First, Rg.Other }, 1);
                    for (const int B : Rg.Head->Bones) Rg.HeadGain += (size_t)B < Mark.size() && InAnim[B] && !Mark[B];
                    MarkBones({ Rg.First, Rg.Other }, 0);
                }
                Rg.S.Score = (float)(Base.Hits + Rg.Gain + Rg.HeadGain) / (float)N;
                Rg.S.Joined = Rg.Other != nullptr;
                return Rg;
            };
            // the models whose rig can be the best, each completed (once per skeleton): the larger of its two
            // parts holds half the bones of a rig fitting as well as the best model alone; of the anim's
            // kind a quarter (a rig of its kind is picked fitting half as well)
            float TopAlone = All.front().Score;
            for (const auto& S : All) if (S.SameGame) { TopAlone = std::min(TopAlone, S.Score); break; }
            const float Least = std::max(0.1f, 0.5f * TopAlone) - 0.02f, LeastOfKind = std::max(0.1f, 0.25f * TopAlone) - 0.02f;
            std::unordered_map<uint64_t, std::pair<const Model*, int>> Done;
            std::vector<Rig> Rigs;
            Rigs.reserve(All.size());
            const auto Whole = [&](const Scored& S) {
                const Model* BaseHead = S.M->RefHead ? nullptr : RefHeadOf(S.M);
                const Model* Second = nullptr;
                int Gain = 0;
                if (S.Score < 0.97f)
                {
                    // the same bones of the same game and kind and role: the same complement (unless it is the
                    // model itself under another folder; a reference body wears its own head)
                    const uint64_t Key = ((uint64_t)S.M->Skel << 24) | ((uint64_t)S.M->Family << 2) | (uint64_t)S.M->R;
                    const auto It = S.M->RefBody ? Done.end() : Done.find(Key);
                    if (It != Done.end() && !(It->second.first && It->second.first->Base == S.M->Base)) { Second = It->second.first; Gain = It->second.second; }
                    else
                    {
                        Second = Complement(S.M, BaseHead, Gain);
                        if (It == Done.end() && !S.M->RefBody) Done[Key] = { Second, Gain };
                    }
                }
                return Assemble(S, Second, Gain);
            };
            // the models completed, sorted again, merged with the others alone (in All's order still)
            std::vector<Rig> Alone;
            for (const auto& S : All)
            {
                if (S.Score >= (S.KindFits ? LeastOfKind : Least)) { Rigs.push_back(Whole(S)); continue; }
                Rig Rg;
                Rg.S = S;
                Rg.First = S.M;
                Alone.push_back(Rg);
            }
            const auto RigBefore = [](const Rig& A, const Rig& B) { return Before(A.S, B.S, true); };
            std::stable_sort(Rigs.begin(), Rigs.end(), RigBefore);
            const size_t Completed = Rigs.size();
            Rigs.insert(Rigs.end(), Alone.begin(), Alone.end());
            std::inplace_merge(Rigs.begin(), Rigs.begin() + (ptrdiff_t)Completed, Rigs.end(), RigBefore);

            // the anim's own game when one of its rigs fits half as well as the best of any game's
            // (a body without its head beside another game's whole one), else any game's
            bool OwnGame = false;
            for (const auto& Rg : Rigs) OwnGame |= Rg.S.SameGame && Rg.S.Score >= 0.2f && Rg.S.Score >= 0.5f * Rigs.front().S.Score;
            // an anim its name says is of a kind (a zombie's, a dog's) plays on a model of that kind fitting
            // half as well as the best: every humanoid shares joint names, not a bind pose (a zombie's walk
            // on a soldier, who has his head where a zombie body has none, twisted his torso)
            bool OwnKind = false;
            const Rig* TopOfGame = nullptr;
            for (const auto& Rg : Rigs)
                if (!OwnGame || Rg.S.SameGame) { TopOfGame = &Rg; break; }
            for (const auto& Rg : Rigs)
                OwnKind |= Rg.S.KindFits && (!OwnGame || Rg.S.SameGame) && Rg.S.Score >= 0.2f && TopOfGame && Rg.S.Score >= 0.5f * TopOfGame->S.Score;
            const auto Eligible = [&](const Rig& Rg) { return (!OwnGame || Rg.S.SameGame) && (!OwnKind || Rg.S.KindFits); };
            const Rig* Best = nullptr;
            for (const auto& Rg : Rigs)
                if (Eligible(Rg)) { Best = &Rg; break; }
            // the bind pose: every CoD humanoid shares joint names, not its bind (BO2's and BO3's zombies
            // have j_spinelower turned 115-121 degrees from a player's), and an anim's rotations replace the
            // bind's, so one played on the other twists its torso. Among the rigs fitting as well as the
            // best (its step, its game and role: the names' first ties), the one whose body binds are
            // nearest the anim's first frame, by more than 30 degrees over those of another bind, is
            // picked; else the names decide (words, fewer bones left). A role the name gives stays (a
            // viewmodel anim on a monster, a soldier's on viewhands, whose fingers or shoulders were nearer)
            std::unordered_set<const Rig*> Kept;   // the rigs of the bind chosen
            if (Best && Best->Whole)
            {
                std::vector<const Rig*> Band;
                for (const auto& Rg : Rigs)
                {
                    if (Step(Rg.S) < Step(Best->S) || Band.size() >= 64) break;
                    if (Eligible(Rg) && Rg.Whole && Rg.S.SameGame == Best->S.SameGame && Rg.S.RoleFits == Best->S.RoleFits) Band.push_back(&Rg);
                }
                if (const Rig* Chosen = PoseChoice(AnimPath, Names, Band, Kept)) Best = Chosen;
            }
            // of a kind, the model sharing more of the anim's words when it fits nearly as well (a zombie's
            // walk on a zombie, not on the leaper having two more of its joints), of the bind chosen
            if (Best && OwnKind)
            {
                const Rig* Top = Best;
                for (const auto& Rg : Rigs)
                    if (Eligible(Rg) && (Kept.empty() || Kept.count(&Rg)) && Rg.S.Score >= 0.9f * Top->S.Score && Rg.S.Words > Best->S.Words) Best = &Rg;
            }
            if (Best == nullptr || Best->S.Score < 0.2f)
            {
                WantRefs = true;
                return R;
            }
            const Rig Pk = Best->Whole ? *Best : Whole(Best->S);

            R.P.Paths.push_back(Pk.First->Path);
            R.P.Label = Pk.First->Base;
            R.P.Reference = Pk.First->Reference;
            for (const Model* Md : { Pk.Other, Pk.Head })
                if (Md)
                {
                    R.P.Paths.push_back(Md->Path);
                    R.P.Label += " + " + Md->Base;
                    R.P.Reference |= Md->Reference;
                }
            // one bone of an anim moving three or more is a coincidence, not a fit (the flare anim's
            // tag_fx on a grenade, its flare bones in no model of the game): nothing is shown
            const int Hits = Pk.S.Hits + Pk.Gain + Pk.HeadGain;
            if (Hits < 2 && Own >= 3)
            {
                R.P = Pick{};
                WantRefs = true;
                return R;
            }
            // the bones no model of its game has: a joint some model of the rig's skeleton has (another game's
            // body holding half the rig's joints, or two thirds of its own in the rig, ten at least: a coat's,
            // the shorts', a face's) is one of its variants', none of the anim's own model's; a dog's palm,
            // whose dogs share 41% of their joints with a body, is not
            std::unordered_set<std::string> Unknown;
            if (!Unmodelled.empty())
            {
                if (RigMark.size() < BoneIds.size()) RigMark.resize(BoneIds.size(), 0);
                int RigN = 0;
                for (const Model* Md : { Pk.First, Pk.Other, Pk.Head })
                    if (Md) for (const int B : Md->Bones) if (!RigMark[B]) { RigMark[B] = 1; RigN++; }
                Shared.assign(Pool.size(), -1);
                for (const auto& [Id, Nm] : Unmodelled)
                {
                    bool Variant = false;
                    if ((size_t)Id < Holders.size())
                        for (const int m : Holders[Id])
                        {
                            if (Shared[m] < 0)
                            {
                                Shared[m] = 0;
                                for (const int B : Pool[m].Bones) Shared[m] += RigMark[B];
                            }
                            if (Shared[m] >= 10 && (2 * Shared[m] >= RigN || 3 * Shared[m] >= 2 * (int)Pool[m].Bones.size())) { Variant = true; break; }
                        }
                    if (!Variant) Unknown.insert(Nm);
                }
                for (const Model* Md : { Pk.First, Pk.Other, Pk.Head })
                    if (Md) for (const int B : Md->Bones) RigMark[B] = 0;
            }
            // of the others, those the anim shapes count against every rig alike (the pick stays): moved, and
            // held off rest with three moved or more, a structure (BO7's Shade: six joints moving, its tongue
            // held, on the generic zombie); keyed at rest only, none (its toes, a dozen hashed joints); held
            // with one moved, none (MWR's face joints beside an NVG's)
            const Shapes Sh = ShapedBones(AnimPath, Unknown);
            const size_t Shaped = Sh.Moving + (Sh.Moving >= 3 ? Sh.Held : 0);
            const float Modelled = (float)N / (float)(N + Shaped);
            R.P.Score = (float)Hits / (float)(N + Shaped);
            R.P.Unmodelled = 1.0f - Modelled;
            R.P.OwnModelAbsent = R.P.Unmodelled >= OwnModelAbsentShare;
            for (auto& C : R.Ranked) C.Score *= Modelled;
            const bool HasView = Pk.First->R == Role::View || (Pk.Other && Pk.Other->R == Role::View);
            WantRefs = R.P.Score < 0.9f || R.P.OwnModelAbsent || (Want == Role::View && !HasView) || !Pk.S.SameGame || (!Kind.empty() && !OwnKind);
            return R;
        }

        void Loop()
        {
            std::unique_lock<std::mutex> Lk(M);
            for (;;)
            {
                Cv.wait(Lk, [&] { return Stop || (HaveModels && !Queue.empty()); });
                if (Stop) return;
                Busy = true;
                const std::string Anim = Queue.back();
                Queue.pop_back();
                Queued.erase(Anim);
                const uint64_t Key = KeyNow();
                if (Key != PoolKey)
                {
                    const std::vector<ModelRef> Models = Listed;
                    const std::map<std::string, Refs> References = GameRefs;
                    Lk.unlock();
                    BuildPool(Models, References);
                    Lk.lock();
                    PoolKey = Key;
                    PoolSize = Pool.size();
                }
                Lk.unlock();
                bool WantRefs = false;
                // the game's own data first (anim_owners.json); the scoring still ranks the models
                Pick Owned;
                std::vector<std::pair<std::string, std::vector<std::string>>> Fetches;
                bool OwnerWantsRefs = false;
                const bool HasOwner = OwnerPick(Anim, Owned, Fetches, OwnerWantsRefs);
                Result R = Choose(Anim, WantRefs);
                if (HasOwner)
                {
                    R.P = std::move(Owned);
                    WantRefs = OwnerWantsRefs;   // its arms in no model here: the game's reference hands
                }
                R.Key = Key;
                const std::string Game = GameOf(Anim);
                Lk.lock();
                // an owner whose models are being fetched: the game's references not asked meanwhile (the
                // anim is picked again once they are in)
                bool OwnerComing = false;
                if (Fetch)
                    for (auto& F : Fetches)
                    {
                        const std::string K = F.first + "|" + Lower(F.second.front());
                        if (OwnerAsked.insert(K).second)
                        {
                            OwnerPending.insert(K);
                            OwnerQueue.push_back(std::move(F));
                            Cv.notify_all();
                        }
                        OwnerComing |= OwnerPending.count(K) != 0;
                    }
                if (!HasOwner && OwnerComing) WantRefs = false;
                Results[Anim] = std::move(R);
                if (WantRefs && Fetch && !Game.empty())
                {
                    Refs& G = GameRefs[Game];
                    if (G.S == Refs::State::None && Table.count(Game))
                    {
                        G.S = Refs::State::Queued;
                        RefQueue.push_back(Game);
                    }
                }
                Busy = false;
                Gen++;
                Cv.notify_all();
            }
        }

        void ReadTable()
        {
            if (TableRead) return;
            TableRead = true;
            const fs::path F = TableFile();
            try
            {
                std::ifstream In(F);
                const nlohmann::json J = nlohmann::json::parse(In);
                for (const auto& K : J.value("kinds", nlohmann::json::array()))
                    Kinds.push_back({ K.at("kind").get<std::string>(), K.value("anims", std::vector<std::string>()),
                                      K.value("models", std::vector<std::string>()) });
                NoKind = J.value("no_kind", std::vector<std::string>());
                OptionalJoints = J.value("optional_joints", std::vector<std::string>());
                for (const auto& [Game, V] : J.at("games").items())
                {
                    if (V.value("from", std::string()) == "install") FromInstall.insert(Game);
                    // a player's lists, then each kind's ("zombie": { "body": [...], "head": [...] })
                    const auto Add = [&](const nlohmann::json& Of, const std::string& Kind) {
                        for (const char* Part : { "hands", "body", "head" })
                            if (Of.contains(Part))
                            {
                                Table[Game].push_back(Of[Part].get<std::vector<std::string>>());
                                if (Part[1] != 'a')
                                    for (const auto& N : Table[Game].back()) (Part[1] == 'o' ? Bodies : Heads)[Game][Lower(N)] = Kind;
                            }
                    };
                    Add(V, std::string());
                    for (const auto& [Key, Of] : V.items())
                        if (Of.is_object()) Add(Of, Key);
                }
            }
            catch (const std::exception& E)
            {
                AppLog::Log().warn("anim rigs: no reference table ({}): {}", F.string(), E.what());
            }
        }

        // what a game's reference folder holds: the first of each list exported there with a mesh
        // (one kept without is exported again)
        std::vector<ModelRef> KeptRefs(const std::string& Game) const
        {
            std::vector<ModelRef> Out;
            const auto It = Table.find(Game);
            if (It == Table.end()) return Out;
            std::error_code Ec;
            for (const auto& List : It->second)
                for (const auto& Name : List)
                {
                    const fs::path F = RigsRoot() / Game / "models" / Name / (Name + "_LOD0.semodel");
                    if (fs::exists(F, Ec) && !MeshlessSEModel(F)) { Out.push_back({ F.string(), Name + "_LOD0" }); break; }
                }
            return Out;
        }

        // the names an export was last tried with (tried.txt): a name the install lacks is not asked again each session.
        // "2|": tried since a model's mesh in another zone is read (a meshless hands model is asked again once)
        static std::string Signature(const Lists& Names)
        {
            std::string S = "2|";
            for (const auto& L : Names)
                for (const auto& N : L) S += N + ",";
            return S;
        }
        static bool Tried(const std::string& Game, const Lists& Names)
        {
            std::ifstream In(RigsRoot() / Game / "tried.txt");
            std::string Line;
            return std::getline(In, Line) && Line == Signature(Names);
        }
        static void MarkTried(const std::string& Game, const Lists& Names)
        {
            std::error_code Ec;
            fs::create_directories(RigsRoot() / Game, Ec);
            std::ofstream(RigsRoot() / Game / "tried.txt") << Signature(Names) << '\n';
        }

        static std::wstring Quote(const std::string& S) { return L"\"" + Strings::ToUnicodeString(S) + L"\""; }

        // disk-export of Names from Zone, as its own process (a reader failing on a zone leaves the browser up)
        bool ExportFrom(const std::string& Game, const std::string& Install, const std::string& Zone,
                               const std::vector<std::string>& Names, std::string& Note)
        {
            std::string Filter;
            for (const auto& N : Names) Filter += (Filter.empty() ? "=" : ",=") + N;   // each name whole
            const fs::path Dir = RigsRoot() / Game;
            std::error_code Ec;
            fs::create_directories(Dir, Ec);
            wchar_t Exe[MAX_PATH * 4] = {};
            GetModuleFileNameW(nullptr, Exe, (DWORD)std::size(Exe));
            std::wstring Cmd = L"\"" + std::wstring(Exe) + L"\" disk-export " + Quote(Zone) + L" --game " + Strings::ToUnicodeString(Game) +
                               L" --install " + Quote(Install) + L" --type model --name-filter " + Quote(Filter) +
                               L" --model-format semodel --image-format png --out " + Quote(RigsRoot().string()) +
                               L" --report " + Quote((Dir / "export_report.txt").string());
            SetEnvironmentVariableW(L"GRAYWOLF_NO_UPDATE", L"1");
            STARTUPINFOW Si{};
            Si.cb = sizeof(Si);
            PROCESS_INFORMATION Pi{};
            std::vector<wchar_t> Mutable(Cmd.begin(), Cmd.end());
            Mutable.push_back(0);
            const std::wstring WorkDir = ExeDir().wstring();
            if (!CreateProcessW(nullptr, Mutable.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, WorkDir.c_str(), &Si, &Pi))
            {
                Note = "could not start disk-export";
                return false;
            }
            bool Done = false;
            for (int Waited = 0; !Done && Waited < 15 * 60 * 5 && !Stopping.load(); Waited++)
                Done = WaitForSingleObject(Pi.hProcess, 200) == WAIT_OBJECT_0;
            if (!Done)
            {
                TerminateProcess(Pi.hProcess, 1);
                WaitForSingleObject(Pi.hProcess, INFINITE);
            }
            DWORD Code = 1;
            GetExitCodeProcess(Pi.hProcess, &Code);
            CloseHandle(Pi.hProcess);
            CloseHandle(Pi.hThread);
            if (!Done || Code != 0) Note = "disk-export of " + Zone + (Done ? " failed (" + std::to_string(Code) + ")" : " timed out");
            return Done && Code == 0;
        }

        // The fewest zones the install index file has the names in (the file read as it is: an index
        // kept before a game update still names zones that are there, and listing the install to
        // check it takes long). False when there is no index file.
        // Skip: per lower-case name, the zones not to take it from (one gave it without its mesh)
        using ZoneSkips = std::map<std::string, std::set<std::string>>;
        static bool ZonesOf(const std::string& IndexFile, const Lists& Names,
                            std::map<std::string, std::vector<std::string>>& ByZone, const ZoneSkips* Skip = nullptr)
        {
            // binary: text mode ends the file at a Ctrl+Z byte (a hashed image name may hold one)
            std::ifstream In(IndexFile, std::ios::binary);
            if (!In) return false;
            std::map<std::string, std::vector<std::string>> Holders;   // lower-case name -> the zones holding it
            for (const auto& List : Names)
                for (const auto& N : List) Holders[Lower(N)];
            std::map<std::string, uint64_t> Sizes;                     // a holding zone's size
            std::string Line, Zone;
            uint64_t Size = 0;
            while (std::getline(In, Line))
            {
                if (!Line.empty() && Line.back() == '\r') Line.pop_back();
                if (Line.rfind("zone\t", 0) == 0)
                {
                    const auto A = Line.find('\t', 5);
                    Size = std::strtoull(Line.c_str() + 5, nullptr, 10);
                    Zone = A == std::string::npos ? std::string() : Line.substr(A + 1);
                }
                else if (Line.rfind("model\t", 0) == 0)
                {
                    const auto It = Holders.find(Lower(Line.substr(6)));
                    if (It == Holders.end() || Zone.empty()) continue;
                    if (Skip)
                        if (const auto S = Skip->find(It->first); S != Skip->end() && S->second.count(Zone)) continue;
                    It->second.push_back(Zone);
                    Sizes.emplace(Zone, Size);
                }
            }
            // each list's first name some zone holds
            std::vector<std::string> Wanted;
            for (const auto& List : Names)
                for (const auto& N : List)
                    if (!Holders[Lower(N)].empty()) { Wanted.push_back(N); break; }
            // the zones taken: each opening costs minutes (a level's global zones loaded beside it), so
            // the zone holding most of the names not placed yet first, the smaller of two holding as
            // many (one name alone: its smallest zone)
            std::vector<bool> Placed(Wanted.size(), false);
            for (size_t Left = Wanted.size(); Left > 0;)
            {
                std::map<std::string, size_t> Count;
                for (size_t i = 0; i < Wanted.size(); i++)
                    if (!Placed[i])
                        for (const auto& Z : Holders[Lower(Wanted[i])]) Count[Z]++;
                const std::string* Take = nullptr;
                for (const auto& [Z, N] : Count)
                    if (Take == nullptr || N > Count[*Take] || (N == Count[*Take] && Sizes[Z] < Sizes[*Take])) Take = &Z;
                if (Take == nullptr) break;
                const std::string Chosen = *Take;
                for (size_t i = 0; i < Wanted.size(); i++)
                {
                    const auto& H = Holders[Lower(Wanted[i])];
                    if (Placed[i] || std::find(H.begin(), H.end(), Chosen) == H.end()) continue;
                    Placed[i] = true;
                    ByZone[Chosen].push_back(Wanted[i]);
                    Left--;
                }
            }
            return true;
        }

        // Game's reference models exported from its install, each from the smallest zone the install index has it in
        // Skip: zones a name is not taken from; From: the zone each name was asked of (lower-case name -> zone)
        void Export(const std::string& Game, const Lists& Names, std::string& Note, const ZoneSkips* Skip = nullptr,
                    std::map<std::string, std::string>* From = nullptr)
        {
            const DiskGame::IDiskGame* G = DiskGame::Find(Game);
            if (G == nullptr) { Note = "no disk reader"; return; }
            static const std::vector<GameInstalls::Install> Detected = GameInstalls::Detect();
            DiskGame::ExportOptions Defaults;
            for (const DiskGame::Setting* S : G->AllSettings()) Defaults.Settings[S->Key] = S->Default;
            // the installs detected, then those an index was kept for (an install no detection finds)
            std::vector<GameInstalls::Install> Installs;
            for (const auto& I : Detected) if (I.DiskId == Game) Installs.push_back(I);
            for (const auto& Path : DiskGame::KeptInstalls(*G, Defaults))
            {
                bool Known = false;
                for (const auto& I : Installs) Known |= Lower(fs::absolute(I.Path).string()) == Lower(Path);
                if (!Known) { GameInstalls::Install I; I.Path = Path; I.DiskId = Game; Installs.push_back(I); }
            }
            if (FromInstall.count(Game))
            {
                // an install read whole (archives, no install index): every name asked of it at once, the
                // first of each list it holds kept
                std::vector<std::string> All;
                for (const auto& List : Names) All.insert(All.end(), List.begin(), List.end());
                Note = "no install found";
                for (const auto& I : Installs)
                {
                    Note.clear();
                    const bool Ok = ExportFrom(Game, I.Path, I.Path, All, Note);
                    AppLog::Log().info("anim rigs: {} references from the install {}: {}", Game, I.Path, Ok ? "exported" : Note);
                    return;
                }
                return;
            }
            Note = "no install index (Load Game: index the install)";
            for (const auto& I : Installs)
            {
                DiskGame::ExportOptions O = Defaults;
                O.Install = I.Path;
                std::map<std::string, std::vector<std::string>> ByZone;
                const std::string IndexFile = DiskGame::KeptFile(*G, O, I.Path, ".idx");
                if (!ZonesOf(IndexFile, Names, ByZone, Skip)) continue;
                AppLog::Log().info("anim rigs: {} references from the index {}: {} zones", Game, IndexFile, ByZone.size());
                if (ByZone.empty()) { Note = "the install index of " + I.Path + " holds none of the names"; continue; }
                Note.clear();
                for (const auto& [Zone, Names] : ByZone)
                {
                    std::vector<std::string> Paths;
                    std::string Error;
                    if (!G->ResolveZones(I.Path, { Zone }, O, Paths, Error) || Paths.empty())
                        Note = Zone + ": " + Error;
                    else
                    {
                        // the zone's file, or its name in the install when it is no file of its own (in an archive)
                        std::error_code Ec;
                        const std::string& Input = fs::exists(fs::path(Strings::ToUnicodeString(Paths[0])), Ec) ? Paths[0] : Zone;
                        const bool Ok = ExportFrom(Game, I.Path, Input, Names, Note);
                        if (From)
                            for (const auto& N : Names) (*From)[Lower(N)] = Zone;
                        AppLog::Log().info("anim rigs: {} reference {} from {}: {}", Game, Names.front(), Input, Ok ? "exported" : Note);
                    }
                }
                return;
            }
        }

        void RefLoop()
        {
            WraithX::InitializeAPI();
            std::unique_lock<std::mutex> Lk(M);
            for (;;)
            {
                Cv.wait(Lk, [&] { return Stop || !RefQueue.empty() || !OwnerQueue.empty(); });
                if (Stop) return;
                RefBusy = true;
                if (RefQueue.empty())
                {
                    // the anims listed picked first (a few seconds at most): their owners' models
                    // asked of the install in one go
                    Cv.wait_for(Lk, std::chrono::seconds(10), [&] { return Stop || !RefQueue.empty() || (Queue.empty() && !Busy); });
                    if (Stop) return;
                    if (!RefQueue.empty()) { RefBusy = false; continue; }
                    FetchOwnerModels(Lk);
                    RefBusy = false;
                    Gen++;
                    Cv.notify_all();
                    continue;
                }
                const std::string Game = RefQueue.front();
                RefQueue.pop_front();
                const Lists Names = Table[Game];
                std::vector<ModelRef> Kept = KeptRefs(Game);
                Lk.unlock();
                std::string Note;
                if (Kept.size() < Names.size() && !Tried(Game, Names))
                {
                    AppLog::Timer T("anim rigs: reference models of " + Game, spdlog::level::info, 0);
                    Export(Game, Names, Note);
                    MarkTried(Game, Names);
                    Lk.lock();
                    Kept = KeptRefs(Game);
                    Lk.unlock();
                    T.Detail(": " + std::to_string(Kept.size()) + " kept" + (Note.empty() ? "" : ", " + Note));
                }
                Lk.lock();
                Refs& R = GameRefs[Game];
                R.S = Refs::State::Done;
                R.Models = std::move(Kept);
                R.Note = Note;
                if (!R.Models.empty())
                {
                    RefVersion++;
                    // the game's anims are picked again with them
                    for (auto& [Anim, Res] : Results)
                        if (GameOf(Anim) == Game && Queued.insert(Anim).second) Queue.push_front(Anim);
                }
                RefBusy = false;
                Gen++;
                Cv.notify_all();
            }
        }

        // The anim_owners.json beside an anim: in its kind's folder (<out>/<game>/anims/), else its own
        const OwnerFile* OwnersOf(const std::string& AnimPath)
        {
            fs::path Dir = fs::path(AnimPath).parent_path();
            for (fs::path D = Dir; !D.empty() && D != D.parent_path(); D = D.parent_path())
                if (Lower(D.filename().string()) == "anims") { Dir = D; break; }
            const std::string File = AnimOwners::FileIn(Dir.string());
            const std::string St = Stamp(File);
            OwnerFile& F = OwnerFiles[File];
            if (F.Stamp != St)
            {
                F = OwnerFile{};
                F.Stamp = St;
                AnimOwners::Book B;
                if (AnimOwners::Read(File, B))
                {
                    std::unordered_map<std::string, uint32_t> At;
                    At.reserve(B.Owners.size());
                    F.Owners.reserve(B.Owners.size());
                    for (auto& [Id, O] : B.Owners)
                    {
                        At.emplace(Id, (uint32_t)F.Owners.size());
                        F.Owners.emplace_back(Id, std::move(O));
                    }
                    F.Anims.reserve(B.Anims.size());
                    for (auto& [Anim, Ids] : B.Anims)
                    {
                        std::vector<uint32_t> Of;
                        Of.reserve(Ids.size());
                        for (const auto& Id : Ids)
                            if (const auto A = At.find(Id); A != At.end()) Of.push_back(A->second);
                        if (!Of.empty()) F.Anims[Lower(Anim)] = std::move(Of);
                    }
                }
            }
            return F.Anims.empty() ? nullptr : &F;
        }

        // An owner's model: the browser's (of the anim's game), else one exported from the install before
        bool OwnerModel(const std::string& Game, const std::string& Name, std::string& Path, bool& Reference)
        {
            if (PoolByNameKey != PoolKey)
            {
                PoolByName.clear();
                for (const Model& Md : Pool)
                    if (!Md.Reference) PoolByName.emplace(Md.Game + "|" + Lower(Md.Base), &Md);
                PoolByNameKey = PoolKey;
            }
            if (const auto It = PoolByName.find(Game + "|" + Lower(Name)); It != PoolByName.end())
            {
                // one the browser holds without its mesh (a level's name-only copy) is no part: the install's then
                const std::string& P = It->second->Path;
                auto [Known, New] = MeshlessOf.try_emplace(P, false);
                if (New && Lower(fs::path(P).extension().string()) == ".semodel") Known->second = MeshlessSEModel(fs::path(Strings::ToUnicodeString(P)));
                if (!Known->second)
                {
                    Path = P;
                    Reference = false;
                    return true;
                }
            }
            std::error_code Ec;
            const fs::path F = RigsRoot() / Game / "models" / Name / (Name + "_LOD0.semodel");
            if (!fs::exists(F, Ec) || MeshlessSEModel(F)) return false;
            Path = F.string();
            Reference = true;
            return true;
        }

        // a joint's id when some model or anim named it, else -1 (none made)
        int KnownBone(const char* Name) const
        {
            const auto It = BoneIds.find(Name);
            return It == BoneIds.end() ? -1 : It->second;
        }
        static bool Holds(const std::vector<int>& Bones, int Id) { return Id >= 0 && std::binary_search(Bones.begin(), Bones.end(), Id); }

        // The anim's first owner whose first model is found, all its models joined: the game gives them as
        // one rig's parts (CoD Online's hands all lie in its sleeves, and are kept). An anim of one part
        // (a facial pf_ anim on a body) plays on it alone; arms no part holds (Black Ops 4's weapons name
        // their gun only) are the game's viewhands, else its reference hands are asked (WantRefs); with
        // no body first, the part holding tag_torso and the shoulders is the skeleton the others join.
        // False when no owner's first model is found: the first owner's missing ones are to be exported.
        bool OwnerPick(const std::string& AnimPath, Pick& Out, std::vector<std::pair<std::string, std::vector<std::string>>>& Fetches,
                       bool& WantRefs)
        {
            WantRefs = false;
            const OwnerFile* F = OwnersOf(AnimPath);
            if (F == nullptr) return false;
            const auto It = F->Anims.find(Lower(fs::path(AnimPath).stem().string()));
            if (It == F->Anims.end()) return false;
            const std::string Game = GameOf(AnimPath);
            // the anim's bones (tag_origin left out, as the scoring does)
            std::vector<int> AnimIds;
            {
                std::vector<std::string> Bones;
                if (AnimBoneNames(AnimPath, Bones))
                    for (const auto& B : Bones)
                        if (_stricmp(B.c_str(), "tag_origin") != 0) AnimIds.push_back(BoneId(B));
                std::sort(AnimIds.begin(), AnimIds.end());
                AnimIds.erase(std::unique(AnimIds.begin(), AnimIds.end()), AnimIds.end());
            }
            const auto HeldBy = [&](const std::vector<int>& Bones) {
                size_t N = 0;
                for (const int B : AnimIds) N += Holds(Bones, B);
                return N;
            };
            const int MainRoot = KnownBone("j_mainroot"), Torso = KnownBone("tag_torso");
            const int ShoulderL = KnownBone("j_shoulder_le"), ShoulderR = KnownBone("j_shoulder_ri");
            std::vector<std::string> Missing;
            for (const uint32_t K : It->second)
            {
                const auto& [Id, O] = F->Owners[K];
                if (O.Models.empty()) continue;
                std::vector<std::string> Paths, Lacks, Names;
                bool Reference = false;
                for (const auto& Name : O.Models)
                {
                    std::string P;
                    bool R = false;
                    if (OwnerModel(Game, Name, P, R)) { Paths.push_back(P); Names.push_back(Name); Reference |= R; }
                    else Lacks.push_back(Name);
                }
                // no first model: the next owner
                if (Paths.empty() || Names.front() != O.Models.front())
                {
                    if (Missing.empty()) Missing = Lacks;
                    continue;
                }
                if (!Lacks.empty() && !Game.empty()) Fetches.push_back({ Game, Lacks });
                std::string Label = Id;
                std::replace(Label.begin(), Label.end(), ':', ' ');

                // an anim of one part: a part holding every bone of the anim the rig holds, the body under half
                if (Paths.size() > 1 && Holds(BonesFor(Paths[0]), MainRoot))
                {
                    std::vector<int> Rig;
                    for (const auto& P : Paths) { const auto& B = BonesFor(P); Rig.insert(Rig.end(), B.begin(), B.end()); }
                    std::sort(Rig.begin(), Rig.end());
                    Rig.erase(std::unique(Rig.begin(), Rig.end()), Rig.end());
                    const size_t All = HeldBy(Rig);
                    if (All >= 3 && 2 * HeldBy(BonesFor(Paths[0])) < All)
                        for (size_t k = 1; k < Paths.size(); k++)
                            if (HeldBy(BonesFor(Paths[k])) == All)
                            {
                                Label += " (" + Names[k] + ")";
                                Paths = { Paths[k] };
                                Names = { Names[k] };
                                break;
                            }
                }

                // arms the anim moves and no part holds: the viewhands holding most of the anim's bones (by
                // name before others, the browser's before a reference, fewer bones left)
                {
                    std::vector<int> Arms;
                    for (const char* A : { "j_shoulder_le", "j_shoulder_ri", "j_elbow_le", "j_elbow_ri", "j_wrist_le", "j_wrist_ri" })
                        if (const int B = KnownBone(A); Holds(AnimIds, B)) Arms.push_back(B);
                    bool Held = false;
                    for (const auto& P : Paths)
                        for (const int B : Arms) Held |= Holds(BonesFor(P), B);
                    if (!Arms.empty() && !Held)
                    {
                        const Model* Best = nullptr;
                        size_t BestHits = 0;
                        for (const Model& Md : Pool)
                        {
                            if (Md.Game != Game || Holds(Md.Bones, MainRoot)) continue;
                            bool Arm = false;
                            for (const int B : Arms) Arm |= Holds(Md.Bones, B);
                            if (!Arm) continue;
                            const size_t Hits = HeldBy(Md.Bones);
                            const bool Better = Best == nullptr                                     ? true
                                                : Hits != BestHits                                  ? Hits > BestHits
                                                : (Md.R == Role::View) != (Best->R == Role::View)   ? Md.R == Role::View
                                                : Md.Reference != Best->Reference                   ? !Md.Reference
                                                : Md.Bones.size() != Best->Bones.size()             ? Md.Bones.size() < Best->Bones.size()
                                                                                                    : Md.Path < Best->Path;
                            if (Better) { Best = &Md; BestHits = Hits; }
                        }
                        if (Best)
                        {
                            Paths.push_back(Best->Path);
                            Label += " + " + Best->Base;
                            Reference |= Best->Reference;
                        }
                        else WantRefs = true;
                    }
                }

                // the arms' chain first (tag_torso -> j_shoulder) when the first part is no body
                if (Paths.size() > 1 && !Holds(BonesFor(Paths[0]), MainRoot))
                {
                    const auto Chain = [&](const std::string& P) {
                        const auto& B = BonesFor(P);
                        return (size_t)(2 * (int)Holds(B, Torso) + (int)Holds(B, ShoulderL) + (int)Holds(B, ShoulderR));
                    };
                    size_t First = 0;
                    for (size_t k = 1, Top = Chain(Paths[0]); k < Paths.size(); k++)
                        if (const size_t C = Chain(Paths[k]); C > Top) { First = k; Top = C; }
                    if (First > 0) std::rotate(Paths.begin(), Paths.begin() + (ptrdiff_t)First, Paths.begin() + (ptrdiff_t)First + 1);
                }

                Out = Pick{};
                Out.Paths = std::move(Paths);
                Out.Reference = Reference;
                Out.Owner = Id;
                Out.Label = std::move(Label);
                std::vector<int> Has;
                for (const auto& P : Out.Paths) { const auto& B = BonesFor(P); Has.insert(Has.end(), B.begin(), B.end()); }
                std::sort(Has.begin(), Has.end());
                Out.Score = AnimIds.empty() ? 0.0f : (float)HeldBy(Has) / (float)AnimIds.size();
                return true;
            }
            if (!Missing.empty() && !Game.empty()) Fetches.push_back({ Game, Missing });
            return false;
        }

        // The owners' models exported from the game's install, as a reference is (Export), once per
        // install's index: those of every owner queued for the game in one go (each disk-export reopens
        // a zone: minutes for a level), the fewest zones holding them; the game's anims then picked
        // again. Called holding M, which it lets go meanwhile.
        void FetchOwnerModels(std::unique_lock<std::mutex>& Lk)
        {
            const std::string Game = OwnerQueue.front().first;
            std::vector<std::string> Names, Keys;
            std::set<std::string> Seen;
            for (auto It = OwnerQueue.begin(); It != OwnerQueue.end();)
            {
                if (It->first != Game) { ++It; continue; }
                Keys.push_back(Game + "|" + Lower(It->second.front()));
                for (const auto& N : It->second)
                    if (Seen.insert(Lower(N)).second) Names.push_back(N);
                It = OwnerQueue.erase(It);
            }
            Lk.unlock();
            // what was asked of the install before (owners_tried.txt): not again each session
            std::set<std::string> Tried;
            {
                std::ifstream In(RigsRoot() / Game / "owners_tried.txt");
                for (std::string Line; std::getline(In, Line);) Tried.insert(Line);
            }
            Lists Asked;
            for (const auto& N : Names)
                if (!Tried.count(Lower(N))) Asked.push_back({ N });
            std::string Note;
            if (!Asked.empty())
            {
                AppLog::Timer T("anim rigs: owner models of " + Game, spdlog::level::info, 0);
                // one a zone gives without its mesh (MW2's viewhands_us_army in mp_terminal) asked of the
                // next zone holding it, two more times at most
                ZoneSkips Skip;
                Lists Now = Asked;
                std::error_code Ec;
                for (int Try = 0; Try < 3 && !Now.empty() && !Stopping.load(); Try++)
                {
                    std::map<std::string, std::string> From;
                    Export(Game, Now, Note, &Skip, &From);
                    Lists Again;
                    for (const auto& L : Now)
                    {
                        const fs::path F = RigsRoot() / Game / "models" / L.front() / (L.front() + "_LOD0.semodel");
                        const auto Z = From.find(Lower(L.front()));
                        if (Z == From.end() || !fs::exists(F, Ec) || !MeshlessSEModel(F)) continue;
                        Skip[Z->first].insert(Z->second);
                        Again.push_back(L);
                    }
                    Now = std::move(Again);
                }
                fs::create_directories(RigsRoot() / Game, Ec);
                std::ofstream Out(RigsRoot() / Game / "owners_tried.txt", std::ios::app);
                for (const auto& L : Asked) Out << Lower(L.front()) << '\n';
                T.Detail(": " + Names.front() + (Note.empty() ? "" : ", " + Note));
            }
            Lk.lock();
            for (const auto& K : Keys) OwnerPending.erase(K);
            // picked again: with the models fetched, else asking the references held back meanwhile
            if (!Asked.empty()) RefVersion++;
            for (auto& [Anim, Res] : Results)
                if (GameOf(Anim) == Game && Queued.insert(Anim).second) Queue.push_front(Anim);
        }

        void Enqueue(const std::string& Anim)
        {
            if (Queued.insert(Anim).second) Queue.push_back(Anim);
            Cv.notify_all();
        }
    };

    Index::Index() : P(std::make_shared<Impl>())
    {
        // references kept by an earlier session are used without asking
        {
            std::lock_guard<std::mutex> Lk(P->M);
            P->ReadTable();
            for (const auto& [Game, Names] : P->Table)
            {
                auto Kept = P->KeptRefs(Game);
                if (Kept.size() >= Names.size() || (!Kept.empty() && Impl::Tried(Game, Names)))
                {
                    P->GameRefs[Game].S = Impl::Refs::State::Done;
                    P->GameRefs[Game].Models = std::move(Kept);
                }
            }
        }
        P->Worker = std::thread([Q = P.get()] { Q->Loop(); });
        P->RefWorker = std::thread([Q = P.get()] { Q->RefLoop(); });
    }

    Index::~Index()
    {
        {
            std::lock_guard<std::mutex> Lk(P->M);
            P->Stop = true;
            P->Stopping = true;
        }
        P->Cv.notify_all();
        if (P->Worker.joinable()) P->Worker.join();
        // a reference export running is waited for (its process, at most its timeout)
        if (P->RefWorker.joinable()) P->RefWorker.join();
    }

    void Index::SetModels(std::vector<ModelRef> Models, uint64_t Version)
    {
        std::lock_guard<std::mutex> Lk(P->M);
        if (P->HaveModels && Version == P->ListedVersion) return;
        P->Listed = std::move(Models);
        P->ListedVersion = Version;
        P->HaveModels = true;
        P->Gen++;
        P->Cv.notify_all();
    }

    bool Index::PickFor(const std::string& AnimPath, Pick& Out)
    {
        std::lock_guard<std::mutex> Lk(P->M);
        const auto It = P->Results.find(AnimPath);
        if (It == P->Results.end() || It->second.Key != P->KeyNow()) P->Enqueue(AnimPath);
        if (It == P->Results.end()) return false;
        Out = It->second.P;
        if (const auto Mn = P->Manual.find(AnimPath); Mn != P->Manual.end())
        {
            Out = Pick{};
            Out.Paths = { Mn->second };
            Out.Manual = true;
            Out.Label = WithoutLod(fs::path(Mn->second).stem().string());
            for (const auto& C : It->second.Ranked)
                if (C.Path == Mn->second) { Out.Score = C.Score; Out.Reference = C.Reference; }
            // no model has those bones: the user's own still lacks them
            Out.Unmodelled = It->second.P.Unmodelled;
            Out.OwnModelAbsent = It->second.P.OwnModelAbsent;
        }
        return true;
    }

    bool Index::Ranked(const std::string& AnimPath, std::vector<Candidate>& Out)
    {
        std::lock_guard<std::mutex> Lk(P->M);
        const auto It = P->Results.find(AnimPath);
        if (It == P->Results.end()) { P->Enqueue(AnimPath); return false; }
        Out = It->second.Ranked;
        return true;
    }

    void Index::SetManual(const std::string& AnimPath, const std::string& ModelPath)
    {
        std::lock_guard<std::mutex> Lk(P->M);
        if (ModelPath.empty()) P->Manual.erase(AnimPath); else P->Manual[AnimPath] = ModelPath;
        P->Gen++;
    }

    uint64_t Index::Generation() const { return P->Gen.load(); }

    void Index::SetFetchReferences(bool On)
    {
        std::lock_guard<std::mutex> Lk(P->M);
        P->Fetch = On;
    }

    void Index::WaitIdle()
    {
        std::unique_lock<std::mutex> Lk(P->M);
        P->Cv.wait(Lk, [&] { return P->Stop || (P->Queue.empty() && !P->Busy && P->RefQueue.empty() && P->OwnerQueue.empty() && !P->RefBusy); });
    }

    std::string Index::Stats() const
    {
        std::lock_guard<std::mutex> Lk(P->M);
        size_t Picks = 0, Rigged = 0, Good = 0, Absent = 0, Owned = 0;
        for (const auto& [Anim, R] : P->Results)
        {
            Picks++;
            Rigged += !R.P.Paths.empty();
            Good += !R.P.Paths.empty() && R.P.Score >= 0.8f;
            Absent += !R.P.Paths.empty() && R.P.OwnModelAbsent;
            Owned += !R.P.Owner.empty();
        }
        std::string S = "anim rigs: " + std::to_string(Picks) + " anims picked, " + std::to_string(Owned) + " by their owners (anim_owners.json), " +
                        std::to_string(Rigged) + " with a rig, " +
                        std::to_string(Good) + " scoring 0.8 or more, " + std::to_string(Absent) + " whose own model is absent, " + std::to_string(P->PoseDecided.load()) + " picks told by the bind pose, " +
                        std::to_string(P->PoolSize) + " models";
        for (const auto& [Game, R] : P->GameRefs)
            S += ", " + Game + " references " + (R.S == Impl::Refs::State::Done ? std::to_string(R.Models.size()) : std::string("queued")) +
                 (R.Note.empty() ? "" : " (" + R.Note + ")");
        return S;
    }
}
