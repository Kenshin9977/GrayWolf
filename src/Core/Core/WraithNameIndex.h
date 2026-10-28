#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// A name file read in place (WraithNameIndex.cpp): its keys sorted, each with its name
class KeptNames;

// The names of a WraithNameIndex, as an unordered_map<uint64_t, std::string> is used: the files
// LoadIndex reads are kept on disk as tables sorted by key and read in place, a lookup reading only
// the keys it compares and copying the one name it finds. A name a run sets, and one looked up
// once, are held in a map before them. Going over every name (begin, size) copies them all first.
// Which name a key gives is the one a map would hold: the last file loaded over the ones before it
// (insert_or_assign), a key set since over every file loaded before, a derived key (AddDerivedKeys:
// try_emplace) under every other.
class NameTable
{
public:
    using Map = std::unordered_map<uint64_t, std::string>;
    using value_type = Map::value_type;

    class const_iterator
    {
    public:
        const_iterator() = default;
        const value_type& operator*() const { return *P; }
        const value_type* operator->() const { return P; }
        const_iterator& operator++()
        {
            ++It;
            P = It == M->end() ? nullptr : &*It;
            return *this;
        }
        bool operator==(const const_iterator& O) const { return P == O.P; }
        bool operator!=(const const_iterator& O) const { return P != O.P; }

    private:
        friend class NameTable;
        const_iterator(const Map* Of, Map::const_iterator At) : M(Of), It(At), P(At == Of->end() ? nullptr : &*At) {}
        const Map* M = nullptr;
        Map::const_iterator It;
        const value_type* P = nullptr;
    };
    using iterator = const_iterator;

    NameTable() = default;
    // (the files are shared, the names held copied)
    NameTable(const NameTable& O) { *this = O; }
    NameTable& operator=(const NameTable& O)
    {
        if (this == &O) return *this;
        std::scoped_lock G(Lock, O.Lock);
        Held = O.Held, All = O.All, Files = O.Files, Derived = O.Derived, Set = O.Set;
        return *this;
    }

    const_iterator find(uint64_t Key) const;
    const_iterator end() const { return {}; }
    const_iterator begin() const;
    size_t size() const;
    const std::string& at(uint64_t Key) const;
    std::string& operator[](uint64_t Key);
    void insert_or_assign(uint64_t Key, std::string Name);
    void try_emplace(uint64_t Key, std::string Name);
    void reserve(size_t) {}
    void clear();

private:
    friend class WraithNameIndex;
    // a file loaded (over the ones before it), or derived keys (under every other)
    void Add(std::shared_ptr<const KeptNames> T, bool Derived);
    // the name of Key in the files, nullptr when none has it (Lock held)
    bool FromFiles(uint64_t Key, std::string& Out) const;
    // every name copied into Held (Lock held)
    void HoldAll() const;

    mutable std::mutex Lock;
    mutable Map Held;
    mutable bool All = true;   // Held holds every name
    std::vector<std::shared_ptr<const KeptNames>> Files, Derived;
    bool Set = false;          // a name set by the run (not only loaded): derived keys are not kept then
};

class WraithNameIndex
{
public:
    WraithNameIndex();
    WraithNameIndex(const std::string& FilePath);

    NameTable NameDatabase;

    void SaveIndex(const std::string& FilePath);
    // KeyMask defaults to the 60-bit XAssetName hash space. BO4's sound-hash ids use 63 bits
    // (bits 60-62 carry data): pass 0x7FFFFFFFFFFFFFFF for that index.
    void LoadIndex(const std::string& LoadIndex, uint64_t KeyMask = 0xFFFFFFFFFFFFFFFull);
    // Each name under another key too, KeyOf(name), where no name is (try_emplace); Tag names the
    // derivation (the derived keys are kept on disk like the files, under the files' and Tag's key)
    void AddDerivedKeys(const std::string& Tag, const std::function<uint64_t(const std::string&)>& KeyOf);
};
