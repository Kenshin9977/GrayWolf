#define NOMINMAX
#include "ZoneTableWalker.h"
#include "ZoneFormat.h"
#include <nlohmann/json.hpp>

#include <intrin.h>
#include <Windows.h>
#include <process.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace ZoneTables
{
namespace
{
    // Python integers are unbounded; 128 bits covers every value a table computes (a u64
    // times a stride), and keeps `u64 == -1` false as in Python.
    struct I128
    {
        uint64_t Lo = 0;
        int64_t Hi = 0;
    };

    I128 FromS(int64_t V) { I128 R; R.Lo = (uint64_t)V; R.Hi = V < 0 ? -1 : 0; return R; }
    I128 FromU(uint64_t V) { I128 R; R.Lo = V; R.Hi = 0; return R; }
    I128 Add(I128 A, I128 B)
    {
        I128 R;
        R.Lo = A.Lo + B.Lo;
        const uint64_t Carry = R.Lo < A.Lo ? 1 : 0;
        R.Hi = (int64_t)((uint64_t)A.Hi + (uint64_t)B.Hi + Carry);
        return R;
    }
    I128 Neg(I128 A) { I128 R; R.Lo = ~A.Lo; R.Hi = ~A.Hi; return Add(R, FromU(1)); }
    I128 Sub(I128 A, I128 B) { return Add(A, Neg(B)); }
    I128 Mul(I128 A, I128 B)
    {
        uint64_t High = 0;
        const uint64_t Low = _umul128(A.Lo, B.Lo, &High);
        High += A.Lo * (uint64_t)B.Hi + (uint64_t)A.Hi * B.Lo;
        I128 R;
        R.Lo = Low;
        R.Hi = (int64_t)High;
        return R;
    }
    int Cmp(I128 A, I128 B)
    {
        if (A.Hi != B.Hi) return A.Hi < B.Hi ? -1 : 1;
        if (A.Lo != B.Lo) return A.Lo < B.Lo ? -1 : 1;
        return 0;
    }
    bool IsZero(I128 A) { return A.Lo == 0 && A.Hi == 0; }
    bool IsNeg(I128 A) { return A.Hi < 0; }
    I128 And(I128 A, I128 B) { I128 R; R.Lo = A.Lo & B.Lo; R.Hi = A.Hi & B.Hi; return R; }
    I128 Or(I128 A, I128 B) { I128 R; R.Lo = A.Lo | B.Lo; R.Hi = A.Hi | B.Hi; return R; }
    I128 Not(I128 A) { I128 R; R.Lo = ~A.Lo; R.Hi = ~A.Hi; return R; }
    I128 Shl(I128 A, int64_t N)
    {
        if (N >= 128) return I128{};
        if (N == 0) return A;
        I128 R;
        if (N >= 64) { R.Hi = (int64_t)(A.Lo << (N - 64)); R.Lo = 0; }
        else { R.Hi = (int64_t)(((uint64_t)A.Hi << N) | (A.Lo >> (64 - N))); R.Lo = A.Lo << N; }
        return R;
    }
    // arithmetic shift written without a signed right shift (implementation-defined pre-C++20)
    int64_t Sar(int64_t V, int64_t N) { return V < 0 ? ~static_cast<int64_t>(~static_cast<uint64_t>(V) >> N) : static_cast<int64_t>(static_cast<uint64_t>(V) >> N); }
    I128 Shr(I128 A, int64_t N)
    {
        if (N >= 128) return A.Hi < 0 ? FromS(-1) : I128{};
        if (N == 0) return A;
        I128 R;
        if (N >= 64) { R.Lo = static_cast<uint64_t>(Sar(A.Hi, N - 64)); R.Hi = A.Hi < 0 ? -1 : 0; }
        else { R.Lo = (A.Lo >> N) | (static_cast<uint64_t>(A.Hi) << (64 - N)); R.Hi = Sar(A.Hi, N); }
        return R;
    }
    bool FitsI64(I128 A)
    {
        return (A.Hi == 0 && (int64_t)A.Lo >= 0) || (A.Hi == -1 && (int64_t)A.Lo < 0);
    }
    int64_t Clamp64(I128 A)
    {
        if (FitsI64(A)) return (int64_t)A.Lo;
        return IsNeg(A) ? INT64_MIN : INT64_MAX;
    }

    struct WalkError : std::runtime_error
    {
        explicit WalkError(const std::string& M) : std::runtime_error(M) {}
    };

    std::string Hex(int64_t V)
    {
        char B[32] = {};
        snprintf(B, sizeof(B), V < 0 ? "-0x%llx" : "0x%llx",
                 (unsigned long long)(V < 0 ? -V : V));
        return B;
    }

    std::string Trim(const std::string& S)
    {
        size_t A = 0, B = S.size();
        while (A < B && isspace((unsigned char)S[A])) A++;
        while (B > A && isspace((unsigned char)S[B - 1])) B--;
        return S.substr(A, B - A);
    }

    // Python int(s, 0): sign, 0x/0o/0b prefixes, decimal without leading zeros, underscores.
    bool ParseInt(const std::string& Text, I128& Out)
    {
        const std::string S = Trim(Text);
        size_t I = 0;
        bool Negative = false;
        if (I < S.size() && (S[I] == '+' || S[I] == '-')) { Negative = S[I] == '-'; I++; }
        int Base = 10;
        if (I + 1 < S.size() && S[I] == '0')
        {
            const char P = (char)tolower((unsigned char)S[I + 1]);
            if (P == 'x') { Base = 16; I += 2; }
            else if (P == 'o') { Base = 8; I += 2; }
            else if (P == 'b') { Base = 2; I += 2; }
        }
        if (I >= S.size()) return false;
        I128 V;
        bool Any = false, NonZero = false, LeadingZero = Base == 10 && S[I] == '0';
        for (; I < S.size(); I++)
        {
            const char C = S[I];
            if (C == '_') continue;
            int D = 0;
            if (C >= '0' && C <= '9') D = C - '0';
            else if (C >= 'a' && C <= 'z') D = C - 'a' + 10;
            else if (C >= 'A' && C <= 'Z') D = C - 'A' + 10;
            else return false;
            if (D >= Base) return false;
            if (D) NonZero = true;
            V = Add(Mul(V, FromU((uint64_t)Base)), FromU((uint64_t)D));
            Any = true;
        }
        if (!Any || (LeadingZero && NonZero)) return false;
        Out = Negative ? Neg(V) : V;
        return true;
    }

    int WidthOf(const std::string& Kind, bool& Signed)
    {
        static const struct { const char* N; int W; bool S; } K[] = {
            {"u8", 1, false}, {"i8", 1, true}, {"u16", 2, false}, {"i16", 2, true},
            {"u32", 4, false}, {"i32", 4, true}, {"u64", 8, false}, {"i64", 8, true}};
        for (const auto& E : K)
            if (Kind == E.N) { Signed = E.S; return E.W; }
        return 0;
    }

    enum Op { OpNone, OpGe, OpLe, OpEq, OpNe, OpShr, OpShl, OpGt, OpLt, OpMul, OpAdd, OpSub,
              OpAnd, OpOr };

    Op OpOf(const std::string& T)
    {
        static const std::pair<const char*, Op> Ops[] = {
            {">=", OpGe}, {"<=", OpLe}, {"==", OpEq}, {"!=", OpNe}, {">>", OpShr},
            {"<<", OpShl}, {">", OpGt}, {"<", OpLt}, {"*", OpMul}, {"+", OpAdd},
            {"-", OpSub}, {"&", OpAnd}, {"|", OpOr}};
        for (const auto& P : Ops)
            if (T == P.first) return P.second;
        return OpNone;
    }

    // A token as the evaluator sees it; a term's meaning is decoded once, here.
    struct Token
    {
        enum Kind { LParen, RParen, LBracket, RBracket, Bracket, Term } K = Term;
        std::string Text;
        Op Oper = OpNone;         // when the token is an operator
        // term
        enum TermKind { Base, Index, Mark, Field, AssetAt, Literal, Bad } TK = Bad;
        int Width = 0;
        bool Signed = false;
        int Up = 0;               // `%` count
        bool Root = false;        // `^`
        I128 Off;                 // field offset, or literal value
        bool OffOk = false;
        std::string Name;         // mark name / bad reason
    };

    void DecodeTerm(Token& T)
    {
        const std::string S = Trim(T.Text);
        if (S == ".") { T.TK = Token::Base; return; }
        if (S == "#") { T.TK = Token::Index; return; }
        if (!S.empty() && S[0] == '$') { T.TK = Token::Mark; T.Name = S.substr(1); return; }
        if (S.rfind("asset@", 0) == 0)
        {
            T.TK = Token::AssetAt;
            T.OffOk = ParseInt(S.substr(6), T.Off);
            return;
        }
        const size_t At = S.find('@');
        if (At != std::string::npos)
        {
            T.TK = Token::Field;
            T.Width = WidthOf(S.substr(0, At), T.Signed);
            std::string Off = S.substr(At + 1);
            if (!Off.empty() && Off[0] == '^') { T.Root = true; Off = Off.substr(1); }
            else
            {
                size_t Up = 0;
                while (Up < Off.size() && Off[Up] == '%') Up++;
                T.Up = (int)Up;
                Off = Off.substr(Up);
            }
            T.OffOk = ParseInt(Off, T.Off);
            if (!T.Width) T.Name = "unknown width " + S.substr(0, At);
            return;
        }
        if (ParseInt(S, T.Off)) { T.TK = Token::Literal; return; }
        T.TK = Token::Bad;
        T.Name = "invalid literal " + S;
    }

    // walker.tokens(), character for character.
    struct Expr
    {
        std::vector<Token> Toks;
        std::string Error;        // tokenizing failed: raised when evaluated
    };

    bool IsAlnum(char C) { return isalnum((unsigned char)C) != 0; }

    Expr Tokenize(const std::string& E)
    {
        Expr X;
        std::vector<std::string> Out;
        size_t I = 0;
        auto IsOpTok = [](const std::string& T) { return OpOf(T) != OpNone; };
        while (I < E.size())
        {
            const char C = E[I];
            if (isspace((unsigned char)C)) { I++; continue; }
            if (C == '(' || C == ')' || C == '[' || C == ']') { Out.push_back(std::string(1, C)); I++; continue; }
            if (I + 1 < E.size())
            {
                const std::string Two = E.substr(I, 2);
                if (Two == ">=" || Two == "<=" || Two == "==" || Two == "!=" || Two == ">>" || Two == "<<")
                {
                    Out.push_back(Two);
                    I += 2;
                    continue;
                }
            }
            if (C == '-' && I + 1 < E.size() && isdigit((unsigned char)E[I + 1])
                && (Out.empty() || IsOpTok(Out.back()) || Out.back() == "(" || Out.back() == "["))
            {
                size_t J = I + 1;
                while (J < E.size() && IsAlnum(E[J])) J++;
                Out.push_back(E.substr(I, J - I));
                I = J;
                continue;
            }
            if (strchr("><*+-&|", C) && C != 0 && !(!Out.empty() && Out.back() == "@"))
            {
                Out.push_back(std::string(1, C));
                I++;
                continue;
            }
            size_t J = I;
            while (J < E.size() && (IsAlnum(E[J]) || strchr("@_^$#%.", E[J])
                                    || (E[J] == '-' && J > I && E[J - 1] == '@')))
                J++;
            if (J == I) { X.Error = std::string("cannot read '") + C + "' in " + E; return X; }
            bool Sg = false;
            if (J < E.size() && E[J] == '[' && WidthOf(E.substr(I, J - I), Sg))
            {
                Out.push_back(E.substr(I, J - I) + "[");
                J++;
            }
            else
                Out.push_back(E.substr(I, J - I));
            I = J;
        }
        for (const auto& S : Out)
        {
            Token T;
            T.Text = S;
            if (S == "(") T.K = Token::LParen;
            else if (S == ")") T.K = Token::RParen;
            else if (S == "[") T.K = Token::LBracket;
            else if (S == "]") T.K = Token::RBracket;
            else if (S.size() > 1 && S.back() == '[')
            {
                T.K = Token::Bracket;
                T.Width = WidthOf(S.substr(0, S.size() - 1), T.Signed);
            }
            else
            {
                T.K = Token::Term;
                T.Oper = OpOf(S);
                DecodeTerm(T);
            }
            X.Toks.push_back(T);
        }
        return X;
    }

    enum Code { CSelf, CElem, CNoRead, CCall, CRead, CReadStr, CMark, CArray, CAlign, CAt, CUnsupported,
                CIf, CElse, CEndIf, CEmit, CSum, CEach };

    struct Arg
    {
        std::string Text;
        I128 Int;
        bool IntOk = false;
        Expr X;
        int Table = -1;           // index when the arg names a table
    };

    struct Action
    {
        std::string Op;           // as written (with suffix)
        std::string Bare;         // suffix stripped
        bool Both = false, Any = false;
        Code C = CEmit;
        std::vector<Arg> Args;    // args after the op
        int SelfIndex = -1;       // unused
    };

    struct Table
    {
        std::string Name;
        std::vector<Action> Actions;
        bool HasSelf = false;
        I128 SelfSize;
        bool SelfOk = false;
        std::string SelfText;
    };
}

struct Tables
{
    std::vector<Table> List;
    std::unordered_map<std::string, int> Index;
    // a type's struct reader by its own name ("### image" then "# = f9f18a0"): reader -> type
    std::unordered_map<std::string, std::string> AssetOf;
};

namespace
{
    std::string RStripSuffix(const std::string& Op)
    {
        size_t E = Op.size();
        while (E > 0 && (Op[E - 1] == '*' || Op[E - 1] == '+')) E--;
        return Op.substr(0, E);
    }

    Code CodeOf(const std::string& Op)
    {
        if (Op == "SELF") return CSelf;
        if (Op == "ELEM") return CElem;
        if (Op == "NOREAD") return CNoRead;
        if (Op == "CALL") return CCall;
        if (Op == "READ") return CRead;
        if (Op == "READSTR") return CReadStr;
        if (Op == "MARK") return CMark;
        if (Op == "SUM") return CSum;
        if (Op == "ARRAY") return CArray;
        if (Op == "ALIGN") return CAlign;
        if (Op == "AT") return CAt;
        if (Op == "EACH") return CEach;
        if (Op == "UNSUPPORTED") return CUnsupported;
        if (Op == "IF") return CIf;
        if (Op == "ELSE") return CElse;
        if (Op == "ENDIF") return CEndIf;
        return CEmit;
    }

    // nested tables: BO4's core_ui nests its DDL 195 reader calls (386 tables) deep;
    // the walk runs on its own thread with a stack sized for it.
    constexpr int kMaxDepth = 1024;
    constexpr unsigned kWalkStack = 256u << 20;

    class Walker
    {
    public:
        // short-lived, one per zone, never copied: it holds references to the zone and tables
        Walker(const std::vector<uint8_t>& B, const Tables& T, const StreamRules& R)
            : Buf(B), Tabs(T), Marker(R.Marker), Resident(R.Resident), SlotMarker(R.SlotMarker), Other(R.Other),
              MaxNibble(R.BackrefMaxNibble),
              Iw6Refs(R.Iw6Refs), BothMarkerAssets(R.BothMarkerAssets), Ptr(R.PointerSize == 4 ? 4 : 8), Shift(R.BackrefShift) {}
        Walker(const Walker&) = delete;
        Walker& operator=(const Walker&) = delete;
        Walker(Walker&&) = delete;
        Walker& operator=(Walker&&) = delete;
        ~Walker() = default;

        const std::vector<uint8_t>& Buf;
        const Tables& Tabs;
        const uint64_t Marker;
        const uint64_t Resident;
        const uint64_t SlotMarker;
        const std::vector<uint64_t> Other;
        const uint64_t MaxNibble;
        const bool Iw6Refs;
        const bool BothMarkerAssets;
        const int Ptr;                // a pointer field's width: 8, or 4 (x86)
        const int Shift;              // x86: the block's bit in a packed back reference
        int Depth = 0;

        // a value an asset slot (ASSET, ASSETS, a `*` action) is followed on
        bool AssetMarker(uint64_t V) const
        {
            return V == Marker || V == Resident ||
                   std::find(Other.begin(), Other.end(), V) != Other.end();
        }

        // the block a packed back reference names: block << 60 | offset + 1, or on x86
        // (v - 1) >> Shift
        uint64_t PackedBlock(uint64_t V) const
        {
            return Ptr == 4 ? ((V - 1) & 0xFFFFFFFFull) >> Shift : V >> 60;
        }
        int64_t PackedOffset(uint64_t V) const
        {
            return Ptr == 4 ? (int64_t)((V - 1) & ((1ull << Shift) - 1)) : (int64_t)((V - 1) & 0x0FFFFFFFFFFFFFFFull);
        }

        // 0, any marker, or a packed (block<<60 | offset) back reference into a real block
        bool KnownPointer(uint64_t V) const
        {
            if (V == 0 || AssetMarker(V)) return true;
            if (Iw6Refs) return (V >> 36) == 0xFDFDFDFull;
            return PackedBlock(V) <= MaxNibble;
        }
        bool HasRoot = false;
        int64_t Root = 0;
        int64_t Index = 0;
        std::vector<int64_t> Bases;
        std::unordered_map<std::string, int64_t> Marks;
        // recorder: the open asset instances, as owner ids (a name is hashed once, on push)
        std::vector<uint16_t> Stack;
        ZoneResult* Out = nullptr;
        std::unordered_map<std::string, uint16_t> OwnerIds;

        int64_t Len() const { return static_cast<int64_t>(Buf.size()); }

        uint16_t OwnerId(const std::string& Owner)
        {
            auto It = OwnerIds.find(Owner);
            if (It != OwnerIds.end()) return It->second;
            const uint16_t Id = static_cast<uint16_t>(Out->OwnerNames.size());
            Out->OwnerNames.push_back(Owner);
            OwnerIds.emplace(Owner, Id);
            return Id;
        }

        bool Layout = false;
        std::vector<int32_t> Open;    // with Layout: the open instances
        const StreamRules* Rules = nullptr;
        // the table action being run, and the stream memory cursors
        std::string WhereTable = "-";
        int WhereIdx = -1;
        std::unordered_map<int, int64_t> Cursor;
        // with Layout, for asset@: per growing block its reads (offset, read index), in placement
        // order (the cursor only grows), and each followed pointer field -> where its data went
        std::unordered_map<int, std::vector<std::pair<int64_t, size_t>>> Placed;
        std::unordered_map<int64_t, int64_t> FieldLinks;
        // with Rules->ListRefs: the asset list, in its block (offset, length) and in the zone
        int ListBlock = -1;
        int64_t ListOff = 0, ListLen = 0, ListZone = 0;

        // asset@: the zone position of the asset the pointer at At
        // names - where it was streamed, or for a back reference what resolve_deref finds: the
        // pointer field the game placed at that block and offset, followed in turn.
        int64_t AssetAt(int64_t At, int Depth) const
        {
            auto L = FieldLinks.find(At);
            if (L != FieldLinks.end()) return L->second;
            const uint64_t V = Pointer(At);
            int B = -1;
            int64_t Off = -1;
            if (Iw6Refs && (V >> 36) == 0xFDFDFDFull)
            {
                B = (int)((V >> 32) & 0xF);
                Off = (int64_t)(V & 0xFFFFFFFFull) - 1;
            }
            else if (!Iw6Refs && V != 0 && !AssetMarker(V))
            {
                if (!Rules->DecodeRef(V, B, Off)) B = -1;
            }
            if (B < 0 || !Layout || Depth > 8) throw WalkError("asset@" + Hex(At) + ": not a back reference the layout resolves");
            if (B == ListBlock && Off >= ListOff && Off < ListOff + ListLen)
                return AssetAt(ListZone + (Off - ListOff), Depth + 1);
            auto It = Placed.find(B);
            if (It == Placed.end()) throw WalkError("asset@" + Hex(At) + ": nothing placed in its block");
            const auto& V2 = It->second;
            auto P = std::upper_bound(V2.begin(), V2.end(), std::make_pair(Off, SIZE_MAX));
            if (P == V2.begin()) throw WalkError("asset@" + Hex(At) + ": before the first read of its block");
            --P;
            const int64_t N = Out->Reads[P->second];
            if (Off - P->first >= (N < 0 ? -N : N)) throw WalkError("asset@" + Hex(At) + ": in no read");
            return AssetAt(Out->ReadPos[P->second] + (Off - P->first), Depth + 1);
        }

        bool Growing(int B) const
        {
            for (int G : Rules->GrowingBlocks)
                if (G == B) return true;
            return false;
        }

        void Place(const std::string& Key, int64_t Size)
        {
            int B = -1, A = 1;
            if (Rules->Blocks)
            {
                auto It = Rules->Blocks->find(Key);
                if (It != Rules->Blocks->end()) { B = It->second.first; A = It->second.second; }
            }
            if (!Growing(B)) { Out->ReadMem.push_back(-1); return; }
            int64_t& C = Cursor[B];
            C = (C + A - 1) / A * A;
            Out->ReadMem.push_back((static_cast<int64_t>(B) << 40) | C);
            Placed[B].emplace_back(C, Out->ReadMem.size() - 1);
            C += Size < 0 ? -Size : Size;
        }

        // stream.pointer_ifs: pointers as IF chains on their field. A taken
        // marker arm (`i64@<off> == <marker>`) is the pointer followed; the chain's `u64@<off> != 0`
        // on a back reference is the pointer resolved: through the pointer it names (resolve_deref)
        // when the chain has a slot-marker arm (a Ptr loader), directly otherwise.
        struct IfTest
        {
            int Kind = 0;   // 0 neither, 1 marker arm, 2 non-null test
            int64_t Off = 0;
            bool Deref = false;
            const std::string* AssetType = nullptr;   // keyed_refs: the type its asset arm reads
        };
        std::unordered_map<const std::string*, IfTest> IfTests;
        // Rules->RewindRefs (Black Ops 7): an IfTest's Kind 3 is the non-null test of a pointer
        // DB_RewindPrepare decides, 4 of an asset pointer. The rewind table's keys (a loaded
        // pointer's low 60 bits) -> where its data is, and the asset pointers named by hash (field,
        // the asset's 63-bit hash), resolved against every instance's hash at +0 once the walk ends
        std::unordered_map<uint64_t, int64_t> RewindKeys;
        std::vector<std::tuple<int64_t, uint64_t, int>> HashRefs;   // field, hash, the type's owner id (-1: any)

        // stream.keyed_refs: the inline arm, as the transcriber writes it or by hand, or an asset
        // pointer's (both markers, -1 and 0x7fff...); the whole text, a prefix match is not one
        static bool InlineArm(const std::string& Test, int Bit, unsigned long long& Off)
        {
            unsigned long long O = 0;
            char Want[128];
            if (sscanf_s(Test.c_str(), "%*[(]u64@0x%llx", &O) != 1) return false;
            for (const char* Form : { "((((u64@0x%llx >> %d) & 0xff) & 1) & 0xff) != 0", "((u64@0x%llx >> %d) & 1) != 0" })
            {
                snprintf(Want, sizeof(Want), Form, O, Bit);
                if (Test == Want) return Off = O, true;
            }
            snprintf(Want, sizeof(Want), "(u64@0x%llx & 0x7fffffffffffffff) == 0x7fffffffffffffff", O);
            if (Test == Want) return Off = O, true;
            return false;
        }

        IfTest ParseIf(const Table& Tb, int Idx, const std::string& Test)
        {
            IfTest T;
            int64_t V = 0;
            unsigned long long Off = 0;
            char Tail = 0;
            const int Bit = Rules ? Rules->InlineBit : 0;
            if (sscanf_s(Test.c_str(), "i64@0x%llx == %lld%c", &Off, &V, &Tail, 1u) == 2 && AssetMarker((uint64_t)V))
                T.Kind = 1;
            else if (Bit > 0 && InlineArm(Test, Bit, Off))
                T.Kind = 1;
            else if (sscanf_s(Test.c_str(), "u64@0x%llx != 0%c", &Off, &Tail, 1u) == 1 && Test.size() > 6 && Test.compare(Test.size() - 5, 5, " != 0") == 0)
                T.Kind = 2;
            T.Off = (int64_t)Off;
            if (T.Kind == 2 && Rules && Rules->RewindRefs)
            {
                // the first test nested in this IF says which pointer it is: DB_RewindPrepare's
                // descriptor (bit 63), or a Ptr loader's name-only test
                char Want[96];
                snprintf(Want, sizeof(Want), "(u64@0x%llx >> 63) != 0", Off);
                const std::string Rewind = Want;
                snprintf(Want, sizeof(Want), "(u64@0x%llx & 0x7fffffffffffffff) != 0x3fffffffffffffff", Off);
                const std::string Asset = Want;
                if ((size_t)Idx + 1 < Tb.Actions.size() && Tb.Actions[(size_t)Idx + 1].C == CIf && !Tb.Actions[(size_t)Idx + 1].Args.empty())
                {
                    const std::string& Next = Tb.Actions[(size_t)Idx + 1].Args[0].Text;
                    if (Next == Rewind) T.Kind = 3;
                    else if (Next == Asset) T.Kind = 4;
                }
                // an asset pointer: the type its inline arm reads (asset names are hashed without
                // their type: an xmodel and a material may share one)
                for (size_t K = (size_t)Idx + 1, Nest = 0; T.Kind == 4 && K < Tb.Actions.size(); K++)
                {
                    const Action& A = Tb.Actions[K];
                    if (A.C == CIf) Nest++;
                    else if (A.C == CEndIf && Nest-- == 0) break;
                    else if (A.C == CArray && A.Args.size() > 2)
                        if (const auto It = Tabs.AssetOf.find(A.Args[2].Text); It != Tabs.AssetOf.end())
                        {
                            T.AssetType = &It->second;
                            break;
                        }
                }
                if (T.Kind != 2) return T;
            }
            if (T.Kind == 2)
            {
                // the arms of this IF, up to its ENDIF
                char Arm[96];
                snprintf(Arm, sizeof(Arm), "i64@0x%llx == %lld", Off, (long long)SlotMarker);
                const std::string SlotArm = Arm;
                // stream.keyed_refs: an asset pointer's chain tests the name-only marker next
                snprintf(Arm, sizeof(Arm), "(u64@0x%llx & 0x7fffffffffffffff) != 0x3fffffffffffffff", Off);
                const std::string NameArm = Arm;
                int Nest = 0;
                for (size_t K = (size_t)Idx + 1; K < Tb.Actions.size() && Nest >= 0; K++)
                {
                    const Action& A = Tb.Actions[K];
                    if (A.C == CIf)
                    {
                        if (!A.Args.empty() && A.Args[0].Text == SlotArm) T.Deref = true;
                        if (Bit > 0 && !A.Args.empty() && A.Args[0].Text == NameArm) T.Deref = true;
                        Nest++;
                    }
                    else if (A.C == CEndIf) Nest--;
                    else if (Bit > 0 && A.C == CArray && A.Args.size() > 2 && T.AssetType == nullptr)
                    {
                        if (const auto It = Tabs.AssetOf.find(A.Args[2].Text); It != Tabs.AssetOf.end()) T.AssetType = &It->second;
                    }
                }
            }
            return T;
        }

        void PointerIf(const Table& Tb, int Idx, const std::string& Test, int64_t Base, int64_t Pos)
        {
            auto It = IfTests.find(&Test);
            if (It == IfTests.end()) It = IfTests.emplace(&Test, ParseIf(Tb, Idx, Test)).first;
            const IfTest& T = It->second;
            if (T.Kind == 0) return;
            const int64_t At = Base + T.Off;
            const uint64_t Head = Pointer(At);
            const int Bit = Rules ? Rules->InlineBit : 0;
            if (T.Kind == 3)
            {
                // DB_RewindPrepare (0x382f020): bit 63 an 8-byte descriptor, then the data; bit 60
                // the data; any other value a key the rewind table holds (0x38cc9d0). A loaded
                // pointer's key is registered unless bit 61 is set (DB_RewindEnd 0x382f4f0)
                constexpr uint64_t KeyMask60 = 0x0FFFFFFFFFFFFFFFull;
                if (!Layout) return;
                if ((Head >> 63) & 1 || (Head >> 60) & 1)
                {
                    const int64_t Target = Pos + (((Head >> 63) & 1) ? 8 : 0);
                    Out->Links.emplace_back(At, Target);
                    FieldLinks[At] = Target;
                    if (!((Head >> 61) & 1))
                    {
                        RewindKeys[Head & KeyMask60] = Target;
                        Out->Keys.emplace_back(Head & KeyMask60, Target);   // for the zones loaded after it
                    }
                    return;
                }
                const auto Found = RewindKeys.find(Head & KeyMask60);
                if (Found != RewindKeys.end())
                {
                    Out->Links.emplace_back(At, Found->second);
                    FieldLinks[At] = Found->second;
                }
                else
                {
                    Out->BackRefs.emplace_back(At, Head);   // in no earlier load of this zone: unresolved
                    Out->BackRefIsAsset.push_back(false);
                }
                return;
            }
            if (T.Kind == 4)
            {
                // a Ptr loader's slot: inline (0x7fff...), a name (0x3fff..., its text follows), or
                // the hash of an asset defined elsewhere (DB_AddAssetRef by hash, 0x3a67770)
                constexpr uint64_t Low = 0x7FFFFFFFFFFFFFFFull;
                if (!Layout) return;
                if ((Head & Low) == Low)
                {
                    Out->Links.emplace_back(At, Pos);
                    FieldLinks[At] = Pos;
                }
                else if ((Head & Low) != 0x3FFFFFFFFFFFFFFFull)
                    HashRefs.emplace_back(At, Head & Low, T.AssetType ? (int)OwnerId(*T.AssetType) : -1);
                return;
            }
            if (T.Kind == 1)
            {
                if (Head == SlotMarker && Bit == 0) NoteResident();
                if (Layout)
                {
                    Out->Links.emplace_back(At, Pos);
                    FieldLinks[At] = Pos;
                    // stream.keyed_refs: the data filed under its key (ResolveStreamPositionEx)
                    if (Bit > 0 && ((Head >> Bit) & 1) != 0 && ((Head >> Rules->NoKeyBit) & 1) == 0)
                        Out->Keys.emplace_back(Head & Rules->KeyMask, Pos);
                }
            }
            else if (Bit > 0)
            {
                // a data pointer without the inline bit, or an asset pointer that is neither marker
                // (-1 / 0x7fff... inline, 0x3fff... name only): a reference by key or by name hash
                const uint64_t Masked = Head & 0x7fffffffffffffffull;
                const bool Ref = T.Deref ? Masked != 0x7fffffffffffffffull && Masked != 0x3fffffffffffffffull
                                         : ((Head >> Bit) & 1) == 0;
                if (Layout && Head != 0 && Ref)
                {
                    Out->BackRefs.emplace_back(At, Head);
                    Out->BackRefIsAsset.push_back(T.Deref);
                    if (T.Deref && T.AssetType) Out->RefTypes[At] = OwnerId(*T.AssetType);
                }
            }
            else if (Layout && !AssetMarker(Head))
            {
                Out->BackRefs.emplace_back(At, Head);
                Out->BackRefIsAsset.push_back(T.Deref);
            }
        }

        void NoteResident()
        {
            if (!Layout) return;
            int64_t& C = Cursor[Rules->ResidentBlock];
            C = (C + Rules->ResidentAlign - 1) / Rules->ResidentAlign * Rules->ResidentAlign + Rules->ResidentSize;
        }

        void Record(I128 N, int64_t At, const std::string* SelfOf = nullptr)
        {
            if (IsZero(N))
            {
                // an empty allocation some readers still make: it aligns the cursor
                if (Layout && !SelfOf && Rules->Empties && Rules->Blocks)
                {
                    const std::string Key = WhereTable + "|" + std::to_string(WhereIdx);
                    auto It = Rules->Blocks->find(Key);
                    if (Rules->Empties->count(Key) && It != Rules->Blocks->end() && Growing(It->second.first))
                    {
                        int64_t& C = Cursor[It->second.first];
                        const int64_t A = It->second.second;
                        C = (C + A - 1) / A * A;
                    }
                }
                return;
            }
            Out->Reads.push_back(Clamp64(N));
            Out->Owners.push_back(Stack.empty() ? OwnerId(Unknown) : Stack.back());
            if (Layout)
            {
                Out->ReadPos.push_back(At);
                Place(SelfOf ? WhereTable + "|" + std::to_string(WhereIdx) + "|" + *SelfOf
                             : WhereTable + "|" + std::to_string(WhereIdx), Clamp64(N));
            }
        }

        void OpenInstance(uint16_t Owner, int64_t Pos)
        {
            if (!Layout) return;
            ZoneResult::Instance I;
            I.Owner = Owner;
            I.Parent = Open.empty() ? -1 : Open.back();
            I.Pos = Pos;
            I.FirstRead = static_cast<uint32_t>(Out->Reads.size());
            Open.push_back(static_cast<int32_t>(Out->Instances.size()));
            Out->Instances.push_back(I);
        }

        void CloseInstance(int64_t End)
        {
            if (!Layout || Open.empty()) return;
            ZoneResult::Instance& I = Out->Instances[static_cast<size_t>(Open.back())];
            I.End = End;
            I.EndRead = static_cast<uint32_t>(Out->Reads.size());
            Open.pop_back();
        }

        I128 ReadAt(int Width, bool Signed, I128 At) const
        {
            if (!Width) throw WalkError("unknown field width");
            if (IsNeg(At) || Cmp(Add(At, FromS(Width)), FromS(Len())) > 0)
                throw WalkError("field past the end");
            const int64_t P = (int64_t)At.Lo;
            uint64_t V = 0;
            memcpy(&V, Buf.data() + P, (size_t)Width);
            if (Signed && Width < 8)
            {
                const int Bits = Width * 8;
                if (V & (1ull << (Bits - 1))) V |= ~0ull << Bits;
                return FromS((int64_t)V);
            }
            return Signed ? FromS((int64_t)V) : FromU(V);
        }

        uint64_t Pointer(int64_t At) const
        {
            if (At < 0 || At + Ptr > Len()) throw WalkError("pointer at " + Hex(At) + " past the end");
            uint64_t V = 0;
            memcpy(&V, Buf.data() + At, (size_t)Ptr);
            return V;
        }

        I128 Term(const Token& T, int64_t Base) const
        {
            switch (T.TK)
            {
            case Token::Base: return FromS(Base);
            case Token::Index: return FromS(Index);
            case Token::Mark:
            {
                auto It = Marks.find(T.Name);
                if (It == Marks.end()) throw WalkError("no mark named " + T.Name);
                return FromS(It->second);
            }
            case Token::AssetAt:
            {
                if (!T.OffOk) throw WalkError("invalid field offset in " + T.Text);
                return FromS(AssetAt(CheckedPos(Add(FromS(Base), T.Off)), 0));
            }
            case Token::Field:
            {
                if (!T.Width) throw WalkError(T.Name);
                if (!T.OffOk) throw WalkError("invalid field offset in " + T.Text);
                if (T.Root)
                {
                    if (!HasRoot) throw WalkError(T.Text + " needs an asset base and there is none");
                    return ReadAt(T.Width, T.Signed, Add(FromS(Root), T.Off));
                }
                if (T.Up)
                {
                    if ((int)Bases.size() <= T.Up)
                        throw WalkError(T.Text + " reaches past the outermost struct");
                    return ReadAt(T.Width, T.Signed,
                                  Add(FromS(Bases[Bases.size() - 1 - T.Up]), T.Off));
                }
                return ReadAt(T.Width, T.Signed, Add(FromS(Base), T.Off));
            }
            case Token::Literal: return T.Off;
            default: throw WalkError(T.Name);
            }
        }

        I128 Eval(const std::vector<Token>& Toks, size_t& I, int64_t Base) const
        {
            I128 V = TermAt(Toks, I, Base);
            while (I < Toks.size() && Toks[I].K != Token::RParen && Toks[I].K != Token::RBracket)
            {
                const Token& O = Toks[I];
                if (O.K != Token::Term || O.Oper == OpNone)
                    throw WalkError("not an operator: " + O.Text);
                I++;
                const I128 B = TermAt(Toks, I, Base);
                V = Apply(O.Oper, V, B);
            }
            return V;
        }

        I128 TermAt(const std::vector<Token>& Toks, size_t& I, int64_t Base) const
        {
            if (I >= Toks.size()) throw WalkError("expression ends early");
            const Token& T = Toks[I];
            if (T.K == Token::LParen)
            {
                I++;
                const I128 V = Eval(Toks, I, Base);
                if (I >= Toks.size() || Toks[I].K != Token::RParen)
                    throw WalkError("unbalanced parentheses");
                I++;
                return V;
            }
            if (T.K == Token::Bracket)
            {
                I++;
                const I128 At = Eval(Toks, I, Base);
                if (I >= Toks.size() || Toks[I].K != Token::RBracket)
                    throw WalkError("unbalanced brackets");
                I++;
                return ReadAt(T.Width, T.Signed, At);
            }
            if (T.K != Token::Term) throw WalkError("invalid literal " + T.Text);
            I++;
            return Term(T, Base);
        }

        static I128 Apply(Op O, I128 A, I128 B)
        {
            switch (O)
            {
            case OpGe: return FromS(Cmp(A, B) >= 0);
            case OpLe: return FromS(Cmp(A, B) <= 0);
            case OpEq: return FromS(Cmp(A, B) == 0);
            case OpNe: return FromS(Cmp(A, B) != 0);
            case OpGt: return FromS(Cmp(A, B) > 0);
            case OpLt: return FromS(Cmp(A, B) < 0);
            case OpMul: return Mul(A, B);
            case OpAdd: return Add(A, B);
            case OpSub: return Sub(A, B);
            case OpAnd: return And(A, B);
            case OpOr: return Or(A, B);
            case OpShl:
            case OpShr:
                if (IsNeg(B)) throw WalkError("negative shift count");
                {
                    const int64_t N = FitsI64(B) ? (int64_t)B.Lo : INT64_MAX;
                    return O == OpShl ? Shl(A, N) : Shr(A, N);
                }
            default: throw WalkError("bad operator");
            }
        }

        I128 Value(const Arg& A, int64_t Base) const
        {
            if (!A.X.Error.empty()) throw WalkError(A.X.Error);
            size_t I = 0;
            return Eval(A.X.Toks, I, Base);
        }

        static I128 IntArg(const Action& A, size_t K)
        {
            if (K >= A.Args.size()) throw WalkError("missing argument for " + A.Op);
            if (!A.Args[K].IntOk) throw WalkError("invalid literal " + A.Args[K].Text);
            return A.Args[K].Int;
        }

        const Arg& ArgAt(const Action& A, size_t K) const
        {
            if (K >= A.Args.size()) throw WalkError("missing argument for " + A.Op);
            return A.Args[K];
        }

        int64_t CheckedPos(I128 P) const
        {
            if (!FitsI64(P)) throw WalkError("position out of range");
            return (int64_t)P.Lo;
        }

        struct DepthGuard
        {
            Walker* W;
            explicit DepthGuard(Walker& X) : W(&X) {}
            DepthGuard(const DepthGuard&) = delete;
            DepthGuard& operator=(const DepthGuard&) = delete;
            DepthGuard(DepthGuard&&) = delete;
            DepthGuard& operator=(DepthGuard&&) = delete;
            ~DepthGuard() { W->Depth--; W->Bases.pop_back(); }
        };

        // walker.Recorder.run + walker.Walker.run
        int64_t Run(int Tbl, const std::string& Name, int64_t Pos, bool HasBase, int64_t Base,
                    bool Consumed)
        {
            if (Tbl < 0) throw WalkError("no table for " + Name);
            const Table& T = Tabs.List[(size_t)Tbl];
            if (!T.Actions.empty() && !Consumed && T.HasSelf)
            {
                if (!T.SelfOk) throw WalkError("invalid literal " + T.SelfText);
                Record(T.SelfSize, Pos, &Name);
            }
            if (Depth > kMaxDepth) throw WalkError("tables recursed too deep");
            Depth++;
            if (!HasBase) Base = Pos;
            Bases.push_back(Base);
            DepthGuard G(*this);
            if (Depth == 1)
            {
                Root = Base;
                HasRoot = true;
                Marks.clear();
            }
            int Skip = 0;
            int Idx = -1;
            for (const Action& A : T.Actions)
            {
                Idx++;
                if (Layout) { WhereTable = Name; WhereIdx = Idx; }
                if (Skip)
                {
                    if (A.C == CIf) Skip++;
                    else if (A.C == CEndIf) Skip--;
                    else if (A.C == CElse && Skip == 1) Skip = 0;
                    continue;
                }
                switch (A.C)
                {
                case CSelf:
                    if (!Consumed) Pos = CheckedPos(Add(FromS(Base), IntArg(A, 0)));
                    break;
                case CElem:
                case CNoRead:
                    break;
                case CCall:
                    Pos = Run(ArgAt(A, 0).Table, ArgAt(A, 0).Text, Pos, true, Base, true);
                    break;
                case CRead:
                {
                    const I128 N = Value(ArgAt(A, 0), Base);
                    if (IsNeg(N) || Cmp(Add(FromS(Pos), N), FromS(Len())) > 0)
                        throw WalkError("READ of " + std::to_string(Clamp64(N)) + " past the end at " + Hex(Pos));
                    Record(N, Pos);
                    Pos += (int64_t)N.Lo;
                    break;
                }
                case CReadStr:
                {
                    // READSTR: a string at the position into a temporary, no field
                    const void* End = Pos < Len() ? memchr(Buf.data() + Pos, 0, (size_t)(Len() - Pos)) : nullptr;
                    if (!End) throw WalkError("unterminated string at " + Hex(Pos));
                    const int64_t After = static_cast<int64_t>(static_cast<const uint8_t*>(End) - Buf.data()) + 1;
                    Record(FromS(-(After - Pos)), Pos);
                    Pos = After;
                    break;
                }
                case CMark:
                    Marks[ArgAt(A, 0).Text] = Pos;
                    break;
                case CSum:
                {
                    // SUM|name|kind|start|count: $name = sum of count values of kind
                    bool Signed = false;
                    const int Width = WidthOf(ArgAt(A, 1).Text, Signed);
                    const I128 Start = Value(ArgAt(A, 2), Base);
                    const int64_t Count = Clamp64(Value(ArgAt(A, 3), Base));
                    I128 Total = FromS(0);
                    for (int64_t K = 0; K < Count; K++)
                        Total = Add(Total, ReadAt(Width, Signed, Add(Start, FromS(K * Width))));
                    Marks[ArgAt(A, 0).Text] = Clamp64(Total);
                    break;
                }
                case CArray:
                {
                    const I128 Count = Value(ArgAt(A, 0), Base);
                    const I128 Stride = IntArg(A, 1);
                    const int64_t First = Pos;
                    const I128 NewPos = Add(FromS(Pos), Mul(Count, Stride));
                    if (Cmp(NewPos, FromS(Len())) > 0 || IsNeg(Count))
                        throw WalkError("inline array of " + std::to_string(Clamp64(Count)) + " x " + std::to_string(Clamp64(Stride)) + " past the end");
                    Pos = (int64_t)NewPos.Lo;
                    const int64_t C = (int64_t)Count.Lo, S = Clamp64(Stride);
                    // stream.pointer_ifs: a Ptr loader reads its asset as one inline struct with the
                    // type's reader: an instance of that type, like an ASSET (its reads keep their owner)
                    const std::string* Type = nullptr;
                    if (Layout && Rules && Rules->PointerIfs && C == 1)
                        if (const auto It = Tabs.AssetOf.find(ArgAt(A, 2).Text); It != Tabs.AssetOf.end()) Type = &It->second;
                    if (Type) OpenInstance(OwnerId(*Type), First);
                    Record(Mul(Count, Stride), First);
                    const int64_t Saved = Index;
                    for (int64_t K = 0; K < C; K++)
                    {
                        Index = K;
                        Pos = Run(ArgAt(A, 2).Table, ArgAt(A, 2).Text, Pos, true, First + K * S, true);
                    }
                    Index = Saved;
                    if (Type) CloseInstance(Pos);
                    break;
                }
                case CAlign:
                {
                    const I128 Mask = IntArg(A, 0);
                    Pos = CheckedPos(And(Add(FromS(Pos), Mask), Not(Mask)));
                    break;
                }
                case CAt:
                    Pos = Run(ArgAt(A, 1).Table, ArgAt(A, 1).Text, Pos, true,
                              CheckedPos(Add(FromS(Base), IntArg(A, 0))), true);
                    break;
                case CEach:
                {
                    // EACH|off|count|stride|table: an inline array, each element in place
                    const I128 Count = Value(ArgAt(A, 1), Base);
                    if (IsNeg(Count)) throw WalkError("negative count " + std::to_string(Clamp64(Count)));
                    const int64_t First = CheckedPos(Add(FromS(Base), IntArg(A, 0)));
                    const int64_t C = Clamp64(Count), S = Clamp64(IntArg(A, 2));
                    const int64_t Saved = Index;
                    for (int64_t K = 0; K < C; K++)
                    {
                        Index = K;
                        Pos = Run(ArgAt(A, 3).Table, ArgAt(A, 3).Text, Pos, true, First + K * S, true);
                    }
                    Index = Saved;
                    break;
                }
                case CUnsupported:
                    throw WalkError(A.Args.empty() ? std::string("UNSUPPORTED") : A.Args[0].Text);
                case CIf:
                    if (IsZero(Value(ArgAt(A, 0), Base))) Skip = 1;
                    else if (Rules && Rules->PointerIfs) PointerIf(T, Idx, ArgAt(A, 0).Text, Base, Pos);
                    break;
                case CElse:
                    Skip = 1;
                    break;
                case CEndIf:
                    break;
                case CEmit:
                    Pos = EmitRecorded(A, Base, Pos);
                    break;
                }
            }
            return Pos;
        }

        // walker.Recorder.emit + walker._list_aware_emit around walker.Walker.emit
        int64_t EmitRecorded(const Action& A, int64_t Base, int64_t Pos)
        {
            if (A.Bare == "STR")
            {
                const int64_t After = Emit(A, Base, Pos);
                if (After != Pos) Record(FromS(-(After - Pos)), Pos);
                return After;
            }
            if (A.Bare == "LIST")
            {
                const int64_t At = CheckedPos(Add(FromS(Base), IntArg(A, 0)));
                const uint64_t Head = Pointer(At);
                const bool Here = A.Any ? Head != 0 : A.Both ? AssetMarker(Head)
                                                             : Head == Marker;
                if (KnownPointer(Head) && Here)
                {
                    const I128 Count = Value(ArgAt(A, 1), Base);
                    Record(Mul(Count, IntArg(A, 2)), Pos);
                }
                return Emit(A, Base, Pos);
            }
            if (A.Bare == "BYTES")
            {
                const int64_t After = Emit(A, Base, Pos);
                // an empty read is an allocation only where the pointer was followed
                if (After != Pos || Followed(A, Base)) Record(FromS(After - Pos), Pos);
                return After;
            }
            return Emit(A, Base, Pos);
        }

        bool Followed(const Action& A, int64_t Base)
        {
            const uint64_t Head = Pointer(CheckedPos(Add(FromS(Base), IntArg(A, 0))));
            if (Iw6Refs && Head != 0 && !AssetMarker(Head)) return false;
            const bool Here = A.Any ? Head != 0 : A.Both ? AssetMarker(Head)
                                                         : Head == Marker;
            return KnownPointer(Head) && Here;
        }

        int64_t Emit(const Action& A, int64_t Base, int64_t Pos)
        {
            const int64_t At = CheckedPos(Add(FromS(Base), IntArg(A, 0)));
            if (A.Bare == "ASSETS")
            {
                const I128 Count = Value(ArgAt(A, 1), Base);
                const int64_t C = Clamp64(Count);
                for (int64_t K = 0; K < C; K++)
                {
                    const uint64_t Slot = Pointer(At + K * Ptr);
                    if (!KnownPointer(Slot))
                        throw WalkError("ASSETS slot " + std::to_string(K) + " holds " + Hex((int64_t)Slot));
                    if (Layout && Slot != 0 && !AssetMarker(Slot))
                    {
                        Out->BackRefs.emplace_back(At + K * Ptr, Slot);
                        Out->BackRefIsAsset.push_back(true);
                    }
                    if (AssetMarker(Slot))
                    {
                        if (Slot == SlotMarker) NoteResident();
                        if (Layout)
                        {
                            Out->Links.emplace_back(At + K * Ptr, Pos);
                            FieldLinks[At + K * Ptr] = Pos;
                        }
                        Pos = Asset(ArgAt(A, 2), Pos);
                    }
                }
                return Pos;
            }
            const uint64_t Head = Pointer(At);
            if (!KnownPointer(Head)) return Pos;
            bool Here = false;
            if (A.Any) Here = Head != 0;
            else if (A.Both || A.Bare == "ASSET") Here = AssetMarker(Head);
            else Here = Head == Marker;
            if (Layout && Head != 0 && !AssetMarker(Head) && !(Rules && Rules->InlineBit > 0 && Here))
            {
                const bool IsAsset = A.Bare == "ASSET" ||
                    (A.Bare == "SUB" && (A.Args.size() > 2 || (A.Both && BothMarkerAssets)));
                // the same field already recorded by its pointer IF (PointerIf)
                if (!Out->BackRefs.empty() && Out->BackRefs.back().first == At) Out->BackRefIsAsset.back() = IsAsset;
                else
                {
                    Out->BackRefs.emplace_back(At, Head);
                    Out->BackRefIsAsset.push_back(IsAsset);
                }
            }
            // an IW6 reference reads nothing, whatever the pointer test (the Python walker never
            // sees it as a pointer at all)
            if (Iw6Refs && Head != 0 && !AssetMarker(Head)) return Pos;
            if (!Here) return Pos;
            if (Head == SlotMarker) NoteResident();
            if (Layout)
            {
                Out->Links.emplace_back(At, Pos);
                FieldLinks[At] = Pos;
            }

            if (A.Bare == "STR")
            {
                const void* End = Pos < Len() ? memchr(Buf.data() + Pos, 0, (size_t)(Len() - Pos)) : nullptr;
                if (!End) throw WalkError("unterminated string at " + Hex(Pos));
                return static_cast<int64_t>(static_cast<const uint8_t*>(End) - Buf.data()) + 1;
            }
            if (A.Bare == "BYTES")
            {
                const I128 N = Value(ArgAt(A, 1), Base);
                if (IsNeg(N) || Cmp(Add(FromS(Pos), N), FromS(Len())) > 0)
                    throw WalkError("BYTES of " + std::to_string(Clamp64(N)) + " past the end at " + Hex(Pos));
                return Pos + (int64_t)N.Lo;
            }
            if (A.Bare == "SUB" && A.Args.size() > 2)
                return AssetInPlace(A, Pos);
            if (A.Bare == "SUB")
                return Run(ArgAt(A, 1).Table, ArgAt(A, 1).Text, Pos, false, 0, false);
            if (A.Bare == "LIST")
            {
                const I128 Count = Value(ArgAt(A, 1), Base);
                const I128 Stride = IntArg(A, 2);
                const int64_t First = Pos;
                if (IsNeg(Count)) throw WalkError("negative count " + std::to_string(Clamp64(Count)));
                const I128 NewPos = Add(FromS(Pos), Mul(Count, Stride));
                if (Cmp(NewPos, FromS(Len())) > 0)
                    throw WalkError("array of " + std::to_string(Clamp64(Count)) + " x " + std::to_string(Clamp64(Stride)) + " past the end");
                Pos = (int64_t)NewPos.Lo;
                const int64_t Saved = Index;
                const int64_t C = (int64_t)Count.Lo, S = Clamp64(Stride);
                for (int64_t K = 0; K < C; K++)
                {
                    Index = K;
                    Pos = Run(ArgAt(A, 3).Table, ArgAt(A, 3).Text, Pos, true, First + K * S, true);
                }
                Index = Saved;
                return Pos;
            }
            if (A.Bare == "SEQ")
            {
                const I128 Count = Value(ArgAt(A, 1), Base);
                if (IsNeg(Count)) throw WalkError("negative count " + std::to_string(Clamp64(Count)));
                const int64_t Saved = Index;
                const int64_t C = Clamp64(Count);
                for (int64_t K = 0; K < C; K++)
                {
                    Index = K;
                    Pos = Run(ArgAt(A, 2).Table, ArgAt(A, 2).Text, Pos, false, 0, false);
                }
                Index = Saved;
                return Pos;
            }
            if (A.Bare == "ASSET")
                return Asset(ArgAt(A, 1), Pos);
            throw WalkError("unknown action " + A.Bare);
        }

        // walker.Recorder.asset + walker.Walker.asset
        int64_t Asset(const Arg& Name, int64_t Pos)
        {
            Stack.push_back(OwnerId(Name.Text));
            OpenInstance(Stack.back(), Pos);
            struct Restore
            {
                Walker* W;
                bool H;
                int64_t R;
                int64_t End = 0;
                Restore(Walker& X, bool Has, int64_t Saved) : W(&X), H(Has), R(Saved) {}
                Restore(const Restore&) = delete;
                Restore& operator=(const Restore&) = delete;
                Restore(Restore&&) = delete;
                Restore& operator=(Restore&&) = delete;
                ~Restore() { W->CloseInstance(End); W->HasRoot = H; W->Root = R; W->Stack.pop_back(); }
            } Rs(*this, HasRoot, Root);
            Root = Pos;
            HasRoot = true;
            Rs.End = Run(Name.Table, Name.Text, Pos, false, 0, false);
            return Rs.End;
        }

        // SUB|off|table|type: an asset of that type its loader reads in place with the type's
        // struct reader: an instance of the type owning its reads,
        // walked as the SUB (its table, its place in the layout, the enclosing asset's root).
        int64_t AssetInPlace(const Action& A, int64_t Pos)
        {
            Stack.push_back(OwnerId(ArgAt(A, 2).Text));
            OpenInstance(Stack.back(), Pos);
            int64_t End = 0;
            try
            {
                End = Run(ArgAt(A, 1).Table, ArgAt(A, 1).Text, Pos, false, 0, false);
            }
            catch (...)
            {
                CloseInstance(0);
                Stack.pop_back();
                throw;
            }
            CloseInstance(End);
            Stack.pop_back();
            return End;
        }

        static const std::string Unknown;
    };

    const std::string Walker::Unknown = "?";
}

Tables* LoadTables(const std::vector<std::string>& Paths)
{
    auto T = std::make_unique<Tables>();
    for (const std::string& Path : Paths)
    {
        std::ifstream In(Path, std::ios::binary);
        if (!In) throw std::runtime_error("cannot open " + Path);
        std::string Line;
        int Current = -1;
        while (std::getline(In, Line))
        {
            if (!Line.empty() && Line.back() == '\r') Line.pop_back();
            if (Line.rfind("###", 0) == 0)
            {
                const std::string Rest = Trim(Line.substr(3));
                const std::string Name = Rest.substr(0, Rest.find_first_of(" \t"));
                auto It = T->Index.find(Name);
                if (It == T->Index.end())
                {
                    Current = (int)T->List.size();
                    T->List.push_back(Table{});
                    T->List.back().Name = Name;
                    T->Index.emplace(Name, Current);
                }
                else
                {
                    Current = It->second;
                    T->List[(size_t)Current] = Table{};
                    T->List[(size_t)Current].Name = Name;
                }
                continue;
            }
            const std::string Stripped = Trim(Line);
            if (Current >= 0 && T->List[(size_t)Current].Actions.empty() && Stripped.rfind("# = ", 0) == 0)
                T->AssetOf[Trim(Stripped.substr(4))] = T->List[(size_t)Current].Name;
            if (Stripped.empty() || Stripped[0] == '#' || Current < 0) continue;
            Action A;
            size_t Start = 0;
            std::vector<std::string> Parts;
            while (true)
            {
                const size_t Bar = Line.find('|', Start);
                Parts.push_back(Trim(Line.substr(Start, Bar == std::string::npos ? std::string::npos : Bar - Start)));
                if (Bar == std::string::npos) break;
                Start = Bar + 1;
            }
            A.Op = Parts[0];
            A.Bare = RStripSuffix(A.Op);
            A.Both = !A.Op.empty() && A.Op.back() == '*';
            A.Any = !A.Op.empty() && A.Op.back() == '+';
            A.C = CodeOf(A.Op);
            for (size_t K = 1; K < Parts.size(); K++)
            {
                Arg G;
                G.Text = Parts[K];
                G.IntOk = ParseInt(G.Text, G.Int);
                G.X = Tokenize(G.Text);
                A.Args.push_back(std::move(G));
            }
            Table& Tb = T->List[(size_t)Current];
            if (A.Op == "SELF" && !Tb.HasSelf)
            {
                Tb.HasSelf = true;
                Tb.SelfText = A.Args.empty() ? "" : A.Args[0].Text;
                Tb.SelfOk = !A.Args.empty() && A.Args[0].IntOk;
                if (Tb.SelfOk) Tb.SelfSize = A.Args[0].Int;
            }
            Tb.Actions.push_back(std::move(A));
        }
    }
    // resolve table names once
    for (Table& Tb : T->List)
        for (Action& A : Tb.Actions)
            for (Arg& G : A.Args)
            {
                auto It = T->Index.find(G.Text);
                G.Table = It == T->Index.end() ? -1 : It->second;
            }
    return T.release();
}

void FreeTables(Tables* T) { delete T; }

static ZoneResult WalkZoneOnThisStack(const std::vector<uint8_t>& Buf, const Tables* T,
                                      const std::vector<std::string>& TypeNames,
                                      const StreamRules& Rules, bool Layout);

ZoneResult WalkZone(const std::vector<uint8_t>& Buf, const Tables* T,
                    const std::vector<std::string>& TypeNames, const StreamRules& Rules, bool Layout)
{
    struct Job
    {
        const std::vector<uint8_t>* Buf = nullptr;
        const Tables* T = nullptr;
        const std::vector<std::string>* TypeNames = nullptr;
        const StreamRules* Rules = nullptr;
        bool Layout = false;
        ZoneResult R;
    } J{&Buf, T, &TypeNames, &Rules, Layout, {}};
    auto Body = [](void* P) -> unsigned {
        Job* X = static_cast<Job*>(P);
        X->R = WalkZoneOnThisStack(*X->Buf, X->T, *X->TypeNames, *X->Rules, X->Layout);
        return 0;
    };
    HANDLE H = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, kWalkStack, Body, &J,
                                                       STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr));
    if (H == nullptr) return WalkZoneOnThisStack(Buf, T, TypeNames, Rules, Layout);
    WaitForSingleObject(H, INFINITE);
    CloseHandle(H);
    return std::move(J.R);
}

static ZoneResult WalkZoneOnThisStack(const std::vector<uint8_t>& Buf, const Tables* T,
                                      const std::vector<std::string>& TypeNames,
                                      const StreamRules& Rules, bool Layout)
{
    ZoneResult R;
    if (T == nullptr)
    {
        R.ParseFailed = true;
        R.Why = "raised: no tables";
        return R;
    }
    // asset@ resolves back references through the layout, so a profile that has one tracks it
    if (Rules.Blocks && !Rules.Blocks->empty()) Layout = true;
    const IZoneFormat* Format = FindZoneFormat(Rules.ZoneFormat);
    if (Format == nullptr)
    {
        R.ParseFailed = true;
        R.Why = "raised: no zone parser for format " + Rules.ZoneFormat;
        return R;
    }
    ZoneHead Head;
    try
    {
        // a prefix read by table (IW7's XGlobals) goes through a walker of its own: its reads
        // are not the assets', as walker.walk keeps them apart
        ZoneResult Scratch;
        Walker Side(Buf, *T, Rules);
        Side.Out = &Scratch;
        Side.Rules = &Rules;
        const PrefixWalk Walk = [&](const std::string& Table, int64_t At) -> int64_t
        {
            auto It = T->Index.find(Table);
            if (It == T->Index.end()) throw std::runtime_error("no table " + Table + " for the zone prefix");
            Side.Depth = 0;
            Side.Bases.clear();
            return Side.Run(It->second, Table, At, false, 0, false);
        };
        Head = Format->Read(Buf, Rules, Walk);
    }
    catch (const std::exception& E)
    {
        R.ParseFailed = true;
        R.Why = std::string("raised: ") + E.what();
        return R;
    }
    const std::vector<ZoneHead::Asset>& Assets = Head.Assets;
    int64_t Pos = Head.BodyStart;
    if (Layout)
    {
        for (const auto& A : Assets) R.AssetHeads.push_back(A.Head);
        R.Start4 = Head.Start + Rules.StartOffset;
    }
    Walker W(Buf, *T, Rules);
    W.Out = &R;
    W.Layout = Layout;
    W.Rules = &Rules;
    if (Layout) W.Cursor[Rules.StartBlock] = R.Start4;
    // the list is the 16 bytes per asset (8 on x86) before the body, and before the start
    // block's cursor
    const int64_t Entry = 2 * (int64_t)(Rules.PointerSize == 4 ? 4 : 8);
    const bool ListRefs = Layout && Rules.ListRefs && !Assets.empty();
    if (ListRefs)
    {
        W.ListBlock = Rules.StartBlock;
        W.ListLen = Entry * (int64_t)Assets.size();
        W.ListOff = R.Start4 - W.ListLen;
        W.ListZone = Head.BodyStart - W.ListLen;
    }
    for (size_t Index = 0; Index < Assets.size(); Index++)
    {
        const ZoneHead::Asset& A = Assets[Index];
        if (A.Head != Rules.Marker && A.Head != Rules.Resident) continue;
        if (ListRefs) W.FieldLinks[W.ListZone + Entry * (int64_t)Index + Entry / 2] = Pos;
        const std::string Name = A.Type >= 0 && A.Type < (int)TypeNames.size() ? TypeNames[(size_t)A.Type] : "";
        auto It = T->Index.find(Name);
        if (It == T->Index.end())
        {
            R.Why = "no table for " + Name;
            return R;
        }
        if (A.Head == Rules.SlotMarker) W.NoteResident();
        W.WhereTable = "-";
        W.WhereIdx = -1;
        W.Stack.push_back(W.OwnerId(Name));
        W.OpenInstance(W.Stack.back(), Pos);
        try
        {
            W.Depth = 0;
            W.Bases.clear();
            Pos = W.Run(It->second, Name, Pos, false, 0, false);
        }
        catch (const std::exception& E)
        {
            while (!W.Open.empty()) W.CloseInstance(Pos);
            R.Why = Name + ": " + std::string(E.what()).substr(0, 70);
            return R;
        }
        W.CloseInstance(Pos);
        W.Stack.pop_back();
    }
    const int64_t Short = (int64_t)Buf.size() - Pos;
    R.Why = Short == 0 ? "walked to the end" : "stopped " + std::to_string(Short) + " bytes short";
    if (Layout) R.BlockEnds = W.Cursor;
    if (Layout && !W.HashRefs.empty())
    {
        // Rules.RewindRefs: an asset pointer by hash names the instance of its type whose hash (+0)
        // it is (names are hashed without their type: an xmodel and a material may share one)
        std::map<std::pair<int, uint64_t>, int64_t> ByHash;
        for (const auto& I : R.Instances)
            if (I.Pos >= 0 && I.Pos + 8 <= (int64_t)Buf.size())
            {
                uint64_t H = 0;
                memcpy(&H, Buf.data() + I.Pos, 8);
                ByHash.try_emplace({ (int)I.Owner, H & 0x7FFFFFFFFFFFFFFFull }, I.Pos);
                ByHash.try_emplace({ -1, H & 0x7FFFFFFFFFFFFFFFull }, I.Pos);
            }
        for (const auto& [Field, Hash, Type] : W.HashRefs)
        {
            const auto It = ByHash.find({ Type, Hash });
            if (It != ByHash.end()) R.Links.emplace_back(Field, It->second);
            else
            {
                R.BackRefs.emplace_back(Field, Hash | 0x8000000000000000ull);
                R.BackRefIsAsset.push_back(true);
                if (Type >= 0) R.RefTypes[Field] = (uint16_t)Type;
            }
        }
    }
    return R;
}

namespace
{
    uint64_t Hex(const nlohmann::json& J, const char* Key)
    {
        return std::stoull(J.at(Key).get<std::string>(), nullptr, 16);
    }

    std::filesystem::path ExeDir()
    {
        wchar_t Buf[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, Buf, MAX_PATH);
        return std::filesystem::path(Buf).parent_path();
    }

    std::filesystem::path FindProfileDir(const std::string& DirOrName)
    {
        namespace fs = std::filesystem;
        const fs::path Direct(DirOrName);
        if (fs::exists(Direct / "profile.json")) return Direct;
        const fs::path Shipped = ExeDir() / "zonekit" / "profiles" / DirOrName;
        if (fs::exists(Shipped / "profile.json")) return Shipped;
        for (fs::path P = ExeDir(); !P.empty() && P != P.parent_path(); P = P.parent_path())
        {
            const fs::path Repo = P / "zonekit" / "profiles" / DirOrName;
            if (fs::exists(Repo / "profile.json")) return Repo;
        }
        return {};
    }
}

bool LoadProfile(const std::string& DirOrName, Profile& Out, std::string& Error)
{
    const std::filesystem::path Dir = FindProfileDir(DirOrName);
    if (Dir.empty())
    {
        Error = "no zonekit profile '" + DirOrName + "' (a directory with profile.json, or a name "
                "under <exe>/zonekit/profiles or <repo>/zonekit/profiles)";
        return false;
    }
    try
    {
        std::ifstream Pf(Dir / "profile.json");
        const nlohmann::json J = nlohmann::json::parse(Pf);
        std::ifstream Tf(Dir / "types.json");
        const nlohmann::json Types = nlohmann::json::parse(Tf);
        Out.Name = J.at("name").get<std::string>();
        Out.Dir = Dir.string();
        const nlohmann::json& S = J.at("stream");
        Out.Rules.Marker = Hex(S, "marker_inline");
        Out.Rules.Resident = Hex(S, "marker_resident");
        Out.Rules.SlotMarker = S.contains("resident_slot") && S.at("resident_slot").contains("marker")
            ? Hex(S.at("resident_slot"), "marker") : Out.Rules.Resident;
        if (S.contains("marker_other"))
            for (const nlohmann::json& V : S.at("marker_other"))
                Out.Rules.Other.push_back(std::stoull(V.get<std::string>(), nullptr, 16));
        Out.Rules.BackrefMaxNibble = Hex(S, "backref_max_nibble");
        Out.Rules.Iw6Refs = S.contains("backref_style") && S.at("backref_style").get<std::string>() == "iw6";
        Out.Rules.PointerSize = J.contains("arch") && J.at("arch").get<std::string>() == "x86" ? 4 : 8;
        if (S.contains("backref_style") && S.at("backref_style").get<std::string>() == "block28")
            Out.Rules.RefShift = 28;
        Out.Rules.Block32 = S.contains("backref_style") && S.at("backref_style").get<std::string>() == "block32";
        Out.Rules.PointerIfs = S.contains("pointer_ifs") && S.at("pointer_ifs").get<bool>();
        Out.Rules.RewindRefs = S.contains("rewind_prepare");
        if (Out.Rules.RewindRefs)
        {
            // the rewind table's keys (the low 60 bits of a loaded pointer) and the assets' name
            // hashes (63 bits): what a reference no zone of its own resolves names (IwBesideZones)
            Out.Rules.KeyMask = 0x0FFFFFFFFFFFFFFFull;
            Out.Rules.AssetHashMask = 0x7FFFFFFFFFFFFFFFull;
        }
        if (S.contains("keyed_refs"))
        {
            const nlohmann::json& K = S.at("keyed_refs");
            Out.Rules.InlineBit = K.at("inline_bit").get<int>();
            Out.Rules.NoKeyBit = K.at("no_key_bit").get<int>();
            Out.Rules.KeyMask = Hex(K, "key_mask");
            Out.Rules.AssetHashMask = Hex(K, "asset_hash_mask");
        }
        if (S.contains("backref_asset_bit")) Out.Rules.AssetBit = S.at("backref_asset_bit").get<int>();
        Out.Rules.StreamBlocks = S.at("blocks").get<int>();
        if (J.contains("export") && J.at("export").contains("both_marker_assets"))
            Out.Rules.BothMarkerAssets = J.at("export").at("both_marker_assets").get<bool>();
        if (S.contains("backref_shift")) Out.Rules.BackrefShift = S.at("backref_shift").get<int>();
        Out.Rules.ZoneFormat = J.at("zone_format").get<std::string>();
        if (J.contains("zone_header"))
        {
            const nlohmann::json& H = J.at("zone_header");
            Out.Rules.HeaderSize = (int64_t)Hex(H, "size");
            Out.Rules.HeaderAssetCount = (int64_t)Hex(H, "asset_count");
            Out.Rules.HeaderAssets = (int64_t)Hex(H, "assets");
        }
        // at least 256 ids; Black Ops 6 and 7 have more (364, 400+)
        int MaxTid = 255;
        for (const auto& [Key, Name] : Types.at("names").items())
            MaxTid = (std::max)(MaxTid, (std::min)(std::stoi(Key), 4095));
        Out.TypeNames.assign((size_t)MaxTid + 1, "");
        const bool ByHash = J.contains("asset_type_map");
        Out.Rules.TypeByNameHash.clear();
        for (const auto& [Key, Name] : Types.at("names").items())
        {
            const int Tid = std::stoi(Key);
            const std::string N = Name.get<std::string>();
            if (Tid >= 0 && Tid <= MaxTid && N.find(' ') == std::string::npos)
                Out.TypeNames[static_cast<size_t>(Tid)] = N;
            if (ByHash && Tid >= 0)
            {
                uint32_t H = 0x811c9dc5u;   // FNV-1a 32 of the type's name
                for (const unsigned char Ch : N) H = (H ^ Ch) * 0x01000193u;
                Out.Rules.TypeByNameHash[H] = Tid;
            }
        }
        Out.Rules.ZoneLoad.clear();
        if (J.contains("zone_load"))
        {
            const std::function<void(const nlohmann::json&, const std::string&)> Flatten =
                [&](const nlohmann::json& O, const std::string& Prefix)
            {
                for (const auto& [K, V] : O.items())
                {
                    if (V.is_object()) Flatten(V, Prefix + K + ".");
                    else if (V.is_number_integer()) Out.Rules.ZoneLoad[Prefix + K] = V.get<int64_t>();
                    else if (V.is_string())
                    {
                        const std::string Sv = V.get<std::string>();
                        try
                        {
                            size_t Used = 0;
                            const int64_t N = (int64_t)std::stoull(Sv, &Used, 0);
                            if (Used == Sv.size()) Out.Rules.ZoneLoad[Prefix + K] = N;
                        }
                        catch (const std::exception&) {}
                    }
                }
            };
            Flatten(J.at("zone_load"), "");
        }
        Out.TablePaths.clear();
        for (const char* Which : {"generated", "hand"})
            Out.TablePaths.push_back((Dir / J.at("tables").at(Which).get<std::string>()).string());
        if (S.contains("growing_blocks"))
            Out.Rules.GrowingBlocks = S.at("growing_blocks").get<std::vector<int>>();
        if (S.contains("resident_slot"))
        {
            const nlohmann::json& Slot = S.at("resident_slot");
            Out.Rules.ResidentBlock = Slot.at("block").get<int>();
            Out.Rules.ResidentSize = Slot.at("size").get<int>();
            Out.Rules.ResidentAlign = Slot.at("align").get<int>();
        }
        // blocks.txt: "key...|block|align", the key being everything before the last two fields
        Out.Blocks.clear();
        const auto& Tb = J.at("tables");
        std::ifstream Bf(Dir / (Tb.contains("blocks") ? Tb.at("blocks").get<std::string>() : std::string("blocks.txt")));
        std::string Line;
        while (std::getline(Bf, Line))
        {
            if (!Line.empty() && Line.back() == '\r') Line.pop_back();
            if (Line.empty() || Line[0] == '#') continue;
            const size_t B2 = Line.rfind('|');
            const size_t B1 = B2 == std::string::npos ? std::string::npos : Line.rfind('|', B2 - 1);
            if (B1 == std::string::npos) continue;
            Out.Blocks[Line.substr(0, B1)] = {std::stoi(Line.substr(B1 + 1, B2 - B1 - 1)),
                                              std::stoi(Line.substr(B2 + 1))};
        }
        Out.Rules.Blocks = &Out.Blocks;
        Out.Empties.clear();
        std::ifstream Ef(Dir / "empties.txt");
        while (std::getline(Ef, Line))
        {
            if (!Line.empty() && Line.back() == '\r') Line.pop_back();
            if (!Line.empty() && Line[0] != '#') Out.Empties.insert(Line);
        }
        Out.Rules.Empties = &Out.Empties;
        if (J.contains("zone_load") && J.at("zone_load").contains("list_block"))
            Out.Rules.StartBlock = J.at("zone_load").at("list_block").get<int>();
        if (J.contains("zone_load") && J.at("zone_load").contains("list_refs"))
            Out.Rules.ListRefs = J.at("zone_load").at("list_refs").get<bool>();
        if (J.contains("zone_load") && J.at("zone_load").contains("prefix_table"))
            Out.Rules.PrefixTable = J.at("zone_load").at("prefix_table").get<std::string>();
    }
    catch (const std::exception& E)
    {
        Error = "profile " + Dir.string() + ": " + E.what();
        return false;
    }
    return true;
}
}

namespace ZoneTables
{
BlockMap::BlockMap(const ZoneResult& Result) : R(Result)
{
    for (size_t K = 0; K < R.ReadMem.size(); K++)
        if (R.ReadMem[K] >= 0)
            ByBlock[static_cast<int>(R.ReadMem[K] >> 40)].emplace_back(R.ReadMem[K] & 0xFFFFFFFFFFll, K);
    for (auto& [B, V] : ByBlock) std::sort(V.begin(), V.end());
}

bool BlockMap::ZonePos(int Block, int64_t Offset, int64_t& Pos) const
{
    auto It = ByBlock.find(Block);
    if (It == ByBlock.end() || Offset < 0) return false;
    const auto& V = It->second;
    auto P = std::upper_bound(V.begin(), V.end(), std::make_pair(Offset, SIZE_MAX));
    if (P == V.begin()) return false;
    --P;
    const int64_t Len = R.Reads[P->second] < 0 ? -R.Reads[P->second] : R.Reads[P->second];
    if (Offset - P->first >= Len) return false;
    Pos = R.ReadPos[P->second] + (Offset - P->first);
    return true;
}
}

namespace ZoneTables
{
namespace
{
    std::unordered_map<std::string, const IZoneFormat*>& ZoneFormats()
    {
        static std::unordered_map<std::string, const IZoneFormat*> Formats;
        return Formats;
    }
}

ZoneFormatRegistration::ZoneFormatRegistration(const IZoneFormat& Format)
{
    ZoneFormats()[Format.Name()] = &Format;
}

const IZoneFormat* FindZoneFormat(const std::string& Name)
{
    auto It = ZoneFormats().find(Name);
    return It == ZoneFormats().end() ? nullptr : It->second;
}
}
