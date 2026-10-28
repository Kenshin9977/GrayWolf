#pragma once

// A zone's bytes and the copy a session serves (VirtualGameSession). The session relocates its
// copy's pointers while the readers keep reading the zone as walked, so the two must differ; a
// zone's bytes are held in a pagefile-backed section (from SectionFrom bytes up), and the session's
// copy is a copy-on-write view of it: no bytes copied, only the pages relocation writes become
// its own. Smaller buffers, and bytes from elsewhere, are copied as before.

#include "ByteView.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

namespace ZoneMemory
{
    inline constexpr size_t SectionFrom = 1 << 20;

    void* Allocate(size_t Bytes);
    void Free(void* P, size_t Bytes);

    template <class T>
    struct Allocator
    {
        using value_type = T;
        Allocator() = default;
        template <class U>
        Allocator(const Allocator<U>&) noexcept {}
        T* allocate(size_t N) { return static_cast<T*>(Allocate(N * sizeof(T))); }
        void deallocate(T* P, size_t N) noexcept { Free(P, N * sizeof(T)); }
        template <class U>
        bool operator==(const Allocator<U>&) const noexcept { return true; }
    };

    // The bytes a session serves: a copy-on-write view of From's section when From starts one,
    // else a copy
    class Copy
    {
    public:
        Copy() = default;
        explicit Copy(ByteView From);
        explicit Copy(std::vector<uint8_t> Owned);
        ~Copy();
        Copy(Copy&& O) noexcept;
        Copy& operator=(Copy&& O) noexcept;
        Copy(const Copy&) = delete;
        Copy& operator=(const Copy&) = delete;

        uint8_t* data() { return Data; }
        const uint8_t* data() const { return Data; }
        size_t size() const { return Size; }
        bool Viewed() const { return View != nullptr; }   // copy-on-write, not copied

    private:
        void Clear();
        std::vector<uint8_t> Owned;
        void* View = nullptr;   // the mapped view, unmapped at the end
        uint8_t* Data = nullptr;
        size_t Size = 0;
    };
}

// A zone's bytes, used as a std::vector<uint8_t> is: storage a session can view copy-on-write
// (ZoneMemory::Copy). A section's pages are zero until written: a growth zeroes only the bytes a
// shorter size had held before (Clean), the rest are left as the section gives them. A decoder
// growing a zone of 650 MB then writing it no longer zeroes it first on one thread, faulting every
// page in before its own threads write them.
class ZoneBytes
{
public:
    using value_type = uint8_t;
    using size_type = size_t;
    using difference_type = std::ptrdiff_t;
    using pointer = uint8_t*;
    using const_pointer = const uint8_t*;
    using reference = uint8_t&;
    using const_reference = const uint8_t&;
    using iterator = uint8_t*;
    using const_iterator = const uint8_t*;

    ZoneBytes() = default;
    explicit ZoneBytes(size_t Count) { resize(Count); }
    template <class It>
    ZoneBytes(It First, It Last) { assign(First, Last); }
    ZoneBytes(const ZoneBytes& O) { assign(O.begin(), O.end()); }
    ZoneBytes(ZoneBytes&& O) noexcept : P(O.P), N(O.N), Cap(O.Cap), Clean(O.Clean) { O.P = nullptr, O.N = O.Cap = O.Clean = 0; }
    ZoneBytes& operator=(const ZoneBytes& O)
    {
        if (this != &O) assign(O.begin(), O.end());
        return *this;
    }
    ZoneBytes& operator=(ZoneBytes&& O) noexcept
    {
        if (this != &O)
        {
            Release();
            P = O.P, N = O.N, Cap = O.Cap, Clean = O.Clean;
            O.P = nullptr, O.N = O.Cap = O.Clean = 0;
        }
        return *this;
    }
    ~ZoneBytes() { Release(); }

    uint8_t* data() noexcept { return P; }
    const uint8_t* data() const noexcept { return P; }
    size_t size() const noexcept { return N; }
    size_t capacity() const noexcept { return Cap; }
    bool empty() const noexcept { return N == 0; }
    uint8_t* begin() noexcept { return P; }
    uint8_t* end() noexcept { return P + N; }
    const uint8_t* begin() const noexcept { return P; }
    const uint8_t* end() const noexcept { return P + N; }
    const uint8_t* cbegin() const noexcept { return P; }
    const uint8_t* cend() const noexcept { return P + N; }
    uint8_t& operator[](size_t K) noexcept { return P[K]; }
    const uint8_t& operator[](size_t K) const noexcept { return P[K]; }
    uint8_t& front() noexcept { return P[0]; }
    uint8_t& back() noexcept { return P[N - 1]; }
    const uint8_t& front() const noexcept { return P[0]; }
    const uint8_t& back() const noexcept { return P[N - 1]; }

    void reserve(size_t Want)
    {
        if (Want > Cap) Move(Want);
    }
    // the bytes added are zero
    void resize(size_t Count)
    {
        if (Count > Cap) Move(std::max(Count, Cap + Cap / 2));
        if (Count > N && N < Clean) std::memset(P + N, 0, std::min(Count, Clean) - N);
        N = Count;
        Clean = std::max(Clean, N);
    }
    void resize(size_t Count, uint8_t Value)
    {
        const size_t Had = N;
        resize(Count);
        if (Count > Had && Value != 0) std::memset(P + Had, Value, Count - Had);
    }
    void clear() noexcept { N = 0; }
    void shrink_to_fit()
    {
        if (Cap > N) Move(N);
    }
    void push_back(uint8_t V)
    {
        if (N == Cap) Move(std::max<size_t>(16, Cap + Cap / 2));
        P[N++] = V;
        Clean = std::max(Clean, N);
    }
    void pop_back() noexcept { --N; }
    template <class It>
    void assign(It First, It Last)
    {
        N = 0;
        insert(end(), First, Last);
    }
    // Last - First bytes at At (input copied in order)
    template <class It>
    uint8_t* insert(const uint8_t* At, It First, It Last)
    {
        const size_t Pos = (size_t)(At - P);
        const size_t Count = (size_t)std::distance(First, Last);
        if (Count == 0) return P + Pos;
        if (N + Count > Cap) Move(std::max(N + Count, Cap + Cap / 2));
        std::memmove(P + Pos + Count, P + Pos, N - Pos);
        std::copy(First, Last, P + Pos);
        N += Count;
        Clean = std::max(Clean, N);
        return P + Pos;
    }
    uint8_t* insert(const uint8_t* At, size_t Count, uint8_t V)
    {
        const size_t Pos = (size_t)(At - P);
        if (N + Count > Cap) Move(std::max(N + Count, Cap + Cap / 2));
        std::memmove(P + Pos + Count, P + Pos, N - Pos);
        std::memset(P + Pos, V, Count);
        N += Count;
        Clean = std::max(Clean, N);
        return P + Pos;
    }
    uint8_t* erase(const uint8_t* First, const uint8_t* Last)
    {
        const size_t Pos = (size_t)(First - P), Count = (size_t)(Last - First);
        std::memmove(P + Pos, P + Pos + Count, N - Pos - Count);
        N -= Count;
        return P + Pos;
    }
    void swap(ZoneBytes& O) noexcept
    {
        std::swap(P, O.P), std::swap(N, O.N), std::swap(Cap, O.Cap), std::swap(Clean, O.Clean);
    }
    bool operator==(const ZoneBytes& O) const { return N == O.N && (N == 0 || std::memcmp(P, O.P, N) == 0); }

private:
    // Want bytes of storage, the bytes held copied
    void Move(size_t Want)
    {
        uint8_t* Q = Want != 0 ? static_cast<uint8_t*>(ZoneMemory::Allocate(Want)) : nullptr;
        if (N != 0) std::memcpy(Q, P, N);
        Release();
        P = Q, Cap = Want;
        // a section is zero past what was copied in; other memory is not known to be
        Clean = Want >= ZoneMemory::SectionFrom ? N : Want;
    }
    void Release() noexcept
    {
        if (P != nullptr) ZoneMemory::Free(P, Cap);
        P = nullptr, Cap = 0;
    }
    uint8_t* P = nullptr;
    size_t N = 0;
    size_t Cap = 0;
    size_t Clean = 0;   // from here on to Cap, the storage is zero (never written)
};
