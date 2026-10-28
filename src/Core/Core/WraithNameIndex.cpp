#include "stdafx.h"

#include "WraithNameIndex.h"

#include "BinaryReader.h"
#include "BinaryWriter.h"
#include "MemoryReader.h"
#include "Compression.h"
#include "FileSystems.h"
#include "Hashing.h"
#include "MappedFile.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace
{
    // bumped when what a kept name file holds changes
    constexpr char NamesMagic[8] = { 'G', 'W', 'N', 'A', 'M', 'E', '1', '\n' };

#pragma pack(push, 1)
    struct Row
    {
        uint64_t Key;
        uint32_t Offset;   // the name's, from the names' start
        uint32_t Length;
    };
#pragma pack(pop)

    // A kept name file: the magic, u64 count, the rows sorted by key (one per key), the names
    bool WriteNames(const std::string& File, std::vector<Row>& Rows, const std::string& Text)
    {
        // another process may read the same file meanwhile
        return FileSystems::WriteReplacing(File, [&](std::ostream& Out) {
            Out.write(NamesMagic, sizeof(NamesMagic));
            const uint64_t Count = Rows.size();
            Out.write(reinterpret_cast<const char*>(&Count), 8);
            Out.write(reinterpret_cast<const char*>(Rows.data()), (std::streamsize)(Rows.size() * sizeof(Row)));
            Out.write(Text.data(), (std::streamsize)Text.size());
        });
    }

    // Names (key, name) to the rows of a kept file, the last of a key kept (as insert_or_assign would)
    void Sorted(std::vector<std::pair<uint64_t, std::string_view>>& Names, std::vector<Row>& Rows, std::string& Text)
    {
        std::stable_sort(Names.begin(), Names.end(), [](const auto& A, const auto& B) { return A.first < B.first; });
        for (size_t K = 0; K < Names.size(); K++)
        {
            if (K + 1 < Names.size() && Names[K + 1].first == Names[K].first) continue;
            Rows.push_back({ Names[K].first, (uint32_t)Text.size(), (uint32_t)Names[K].second.size() });
            Text.append(Names[K].second);
        }
    }

    std::string KeptDir() { return (std::filesystem::temp_directory_path() / "greyhound_names").string(); }
}

class KeptNames
{
public:
    static std::shared_ptr<const KeptNames> Open(const std::string& File)
    {
        MappedFile Map;
        if (!Map.Open(File) || Map.size() < sizeof(NamesMagic) + 8) return nullptr;
        auto K = std::shared_ptr<KeptNames>(new KeptNames(std::move(Map), File));
        const uint8_t* Base = K->Map.data();
        uint64_t Count = 0;
        std::memcpy(&Count, Base + sizeof(NamesMagic), 8);
        const size_t Left = K->Map.size() - sizeof(NamesMagic) - 8;
        if (std::memcmp(Base, NamesMagic, sizeof(NamesMagic)) != 0 || Count > Left / sizeof(Row)) return nullptr;
        K->Rows = reinterpret_cast<const Row*>(Base + sizeof(NamesMagic) + 8);
        K->Count = (size_t)Count;
        K->Text = reinterpret_cast<const char*>(K->Rows + Count);
        K->TextSize = Left - Count * sizeof(Row);
        return K;
    }
    KeptNames(const KeptNames&) = delete;
    KeptNames& operator=(const KeptNames&) = delete;

    bool Find(uint64_t Key, std::string& Out) const
    {
        const Row* End = Rows + Count;
        const Row* It = std::lower_bound(Rows, End, Key, [](const Row& R, uint64_t K) { return R.Key < K; });
        if (It == End || It->Key != Key || (size_t)It->Offset + It->Length > TextSize) return false;
        Out.assign(Text + It->Offset, It->Length);
        return true;
    }
    template <class Fn>
    void Each(Fn&& F) const
    {
        for (size_t K = 0; K < Count; K++)
            if ((size_t)Rows[K].Offset + Rows[K].Length <= TextSize) F(Rows[K].Key, std::string_view(Text + Rows[K].Offset, Rows[K].Length));
    }
    const std::string File;

private:
    KeptNames(MappedFile M, std::string F) : File(std::move(F)), Map(std::move(M)) {}
    MappedFile Map;
    const Row* Rows = nullptr;
    size_t Count = 0;
    const char* Text = nullptr;
    size_t TextSize = 0;
};

bool NameTable::FromFiles(uint64_t Key, std::string& Out) const
{
    for (auto It = Files.rbegin(); It != Files.rend(); ++It)
        if ((*It)->Find(Key, Out)) return true;
    for (const auto& D : Derived)
        if (D->Find(Key, Out)) return true;
    return false;
}

NameTable::const_iterator NameTable::find(uint64_t Key) const
{
    std::lock_guard<std::mutex> G(Lock);
    if (const auto It = Held.find(Key); It != Held.end() || All) return It == Held.end() ? const_iterator() : const_iterator(&Held, It);
    std::string Name;
    if (!FromFiles(Key, Name)) return {};
    return const_iterator(&Held, Held.emplace(Key, std::move(Name)).first);
}

void NameTable::HoldAll() const
{
    if (All) return;
    // the derived keys first, the files over them, the latest over the ones before; what Held has
    // stays (it has its key's name already)
    Map Every;
    for (auto It = Derived.rbegin(); It != Derived.rend(); ++It)
        (*It)->Each([&](uint64_t K, std::string_view N) { Every.insert_or_assign(K, std::string(N)); });
    for (const auto& F : Files) F->Each([&](uint64_t K, std::string_view N) { Every.insert_or_assign(K, std::string(N)); });
    for (auto& [K, N] : Every) Held.try_emplace(K, std::move(N));
    All = true;
}

NameTable::const_iterator NameTable::begin() const
{
    std::lock_guard<std::mutex> G(Lock);
    HoldAll();
    return const_iterator(&Held, Held.begin());
}

size_t NameTable::size() const
{
    std::lock_guard<std::mutex> G(Lock);
    HoldAll();
    return Held.size();
}

const std::string& NameTable::at(uint64_t Key) const
{
    const auto It = find(Key);
    if (It == end()) throw std::out_of_range("no name for this key");
    return It->second;
}

std::string& NameTable::operator[](uint64_t Key)
{
    find(Key);   // a file's name first, when one has it
    std::lock_guard<std::mutex> G(Lock);
    Set = true;
    return Held[Key];
}

void NameTable::insert_or_assign(uint64_t Key, std::string Name)
{
    std::lock_guard<std::mutex> G(Lock);
    Set = true;
    Held.insert_or_assign(Key, std::move(Name));
}

void NameTable::try_emplace(uint64_t Key, std::string Name)
{
    if (find(Key) != end()) return;
    std::lock_guard<std::mutex> G(Lock);
    Set = true;
    Held.try_emplace(Key, std::move(Name));
}

void NameTable::clear()
{
    std::lock_guard<std::mutex> G(Lock);
    Held.clear();
    Files.clear();
    Derived.clear();
    All = true;
    Set = false;
}

void NameTable::Add(std::shared_ptr<const KeptNames> T, bool IsDerived)
{
    std::lock_guard<std::mutex> G(Lock);
    // a name held for a key the file has: the file's (it loads over it); a derived one stays under
    if (!IsDerived)
        for (auto& [K, N] : Held) T->Find(K, N);
    (IsDerived ? Derived : Files).push_back(std::move(T));
    All = false;
}

WraithNameIndex::WraithNameIndex()
{
}

WraithNameIndex::WraithNameIndex(const std::string& FilePath)
{
    this->LoadIndex(FilePath);
}

void WraithNameIndex::SaveIndex(const std::string& FilePath)
{
    auto Writer = BinaryWriter();
    Writer.Create(FilePath);

    Writer.Write<uint32_t>(0x20494E57);
    Writer.Write<uint16_t>(0x1);

    Writer.Write<uint32_t>((uint32_t)NameDatabase.size());

    uint32_t PackageSize = 0;

    for (auto& KeyValue : NameDatabase)
    {
        // Add 8 for key and then string size
        PackageSize += 8 + (uint32_t)(KeyValue.second.size() + 1);
    }

    auto UnpackedData = new int8_t[PackageSize];

    uint32_t DataOffset = 0;

    for (auto& KeyValue : NameDatabase)
    {
        std::memcpy(UnpackedData + DataOffset, &KeyValue.first, 8);
        DataOffset += 8;
        std::memcpy(UnpackedData + DataOffset, KeyValue.second.c_str(), KeyValue.second.size() + 1);
        DataOffset += (uint32_t)(KeyValue.second.size() + 1);
    }

    // Allocate a buffer for compressed (At least 1 / 2 of size in addition)
    auto PackedData = new int8_t[PackageSize + (PackageSize / 2)];

    auto ResultSize = Compression::CompressLZ4Block(UnpackedData, PackedData, PackageSize, PackageSize + (PackageSize / 2));

    delete[] UnpackedData;

    if (ResultSize > 0)
    {
        Writer.Write<uint32_t>(ResultSize);
        Writer.Write<uint32_t>(PackageSize);

        Writer.Write(PackedData, ResultSize);
    }

    delete[] PackedData;
}

void WraithNameIndex::LoadIndex(const std::string& LoadIndex, uint64_t KeyMask)
{
    std::error_code Ec;
    if (!std::filesystem::is_regular_file(LoadIndex, Ec)) return;
    // kept as a file read in place, under the file's place, size and time and the mask
    const auto Size = std::filesystem::file_size(LoadIndex, Ec);
    const auto Time = std::filesystem::last_write_time(LoadIndex, Ec).time_since_epoch().count();
    std::string Where = std::filesystem::absolute(LoadIndex, Ec).string();
    std::transform(Where.begin(), Where.end(), Where.begin(), [](unsigned char C) { return (char)std::tolower(C); });
    const std::string Kept = (std::filesystem::path(KeptDir()) /
                              (std::filesystem::path(LoadIndex).stem().string() + "_" +
                               Hashing::Hex16(Hashing::Fnv1a(Where + '\n' + std::to_string(Size) + '\n' + std::to_string(Time) + '\n' + std::to_string(KeyMask))) + ".gwn"))
                                 .string();
    if (auto K = KeptNames::Open(Kept))
    {
        NameDatabase.Add(std::move(K), false);
        return;
    }

    auto Reader = BinaryReader();
    Reader.Open(LoadIndex);

    // Verify magic ('WNI ')
    if (Reader.Read<uint32_t>() != 0x20494E57) return;
    // Skip over version (Should be 0x1)
    Reader.Advance(2);

    auto EntryCount = Reader.Read<uint32_t>();

    auto Packed = Reader.Read<uint32_t>();
    auto Unpacked = Reader.Read<uint32_t>();

    uint64_t CompressedResult = 0;
    auto CompressedBuffer = Reader.Read(Packed, CompressedResult);
    if (CompressedBuffer == nullptr) return;
    std::vector<int8_t> Decompressed(Unpacked);
    const auto DecompressResult = Compression::DecompressLZ4Block(CompressedBuffer, Decompressed.data(), Packed, Unpacked);
    delete[] CompressedBuffer;
    if (DecompressResult == 0) return;

    // the entries {u64 key, name\0}, a later one replacing one of the same key
    std::vector<std::pair<uint64_t, std::string_view>> Names;
    Names.reserve(EntryCount);
    auto MemReader = MemoryReader(Decompressed.data(), DecompressResult, true);
    const char* Text = reinterpret_cast<const char*>(Decompressed.data());
    for (uint32_t i = 0; i < EntryCount; i++)
    {
        auto Key = MemReader.Read<uint64_t>() & KeyMask;
        const size_t From = (size_t)MemReader.GetPosition();
        const auto Value = MemReader.ReadNullTerminatedString();
        Names.emplace_back(Key, std::string_view(Text + From, Value.size()));
    }
    std::vector<Row> Rows;
    std::string All;
    Sorted(Names, Rows, All);
    if (WriteNames(Kept, Rows, All))
        if (auto K = KeptNames::Open(Kept))
        {
            NameDatabase.Add(std::move(K), false);
            return;
        }
    // (not kept: held)
    for (const auto& [Key, Name] : Names) NameDatabase.insert_or_assign(Key, std::string(Name));
}

void WraithNameIndex::AddDerivedKeys(const std::string& Tag, const std::function<uint64_t(const std::string&)>& KeyOf)
{
    // kept under the files it derives from (a name set by the run is not in them: derived in place)
    std::string Sources = Tag;
    bool Keep = true;
    {
        std::lock_guard<std::mutex> G(NameDatabase.Lock);
        Keep = !NameDatabase.Set && NameDatabase.Derived.empty() && !NameDatabase.Files.empty();
        for (const auto& F : NameDatabase.Files) Sources += '\n' + F->File;
    }
    const std::string Kept = (std::filesystem::path(KeptDir()) / ("derived_" + Hashing::Hex16(Hashing::Fnv1a(Sources)) + ".gwn")).string();
    if (Keep)
        if (auto K = KeptNames::Open(Kept))
        {
            NameDatabase.Add(std::move(K), true);
            return;
        }
    std::vector<std::pair<uint64_t, std::string>> Wide;
    for (const auto& [Key, Name] : NameDatabase) Wide.emplace_back(KeyOf(Name), Name);
    if (Keep)
    {
        std::vector<std::pair<uint64_t, std::string_view>> Names;
        Names.reserve(Wide.size());
        // the first of a key kept (as try_emplace would): reversed, the sort keeps the last
        for (auto It = Wide.rbegin(); It != Wide.rend(); ++It) Names.emplace_back(It->first, It->second);
        std::vector<Row> Rows;
        std::string All;
        Sorted(Names, Rows, All);
        if (WriteNames(Kept, Rows, All))
            if (auto K = KeptNames::Open(Kept))
            {
                NameDatabase.Add(std::move(K), true);
                return;
            }
    }
    for (auto& [Key, Name] : Wide) NameDatabase.try_emplace(Key, std::move(Name));
}
