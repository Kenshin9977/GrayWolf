#include "stdafx.h"

#include "ZoneMemory.h"

#include <Windows.h>

#include <cstring>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace
{
    // each section a buffer is held in, by the address its view starts at
    struct Section
    {
        HANDLE Mapping = nullptr;
        size_t Bytes = 0;
    };
    std::mutex Lock;
    std::map<const void*, Section> Sections;

    // the section whose view starts at P and holds at least N bytes, nullptr when none
    HANDLE SectionOf(const void* P, size_t N)
    {
        const std::lock_guard<std::mutex> Hold(Lock);
        const auto It = Sections.find(P);
        return It != Sections.end() && It->second.Bytes >= N ? It->second.Mapping : nullptr;
    }

    // The views freed, unmapped and their sections closed one after the other on one thread. Never
    // destroyed (the process may end with some queued: the system unmaps them).
    class Unmapping
    {
    public:
        Unmapping()
        {
            std::thread([this] {
                for (;;)
                {
                    std::pair<void*, HANDLE> Next;
                    {
                        std::unique_lock<std::mutex> Hold(QueueLock);
                        Ready.wait(Hold, [this] { return !Queue.empty(); });
                        Next = Queue.front();
                        Queue.pop_front();
                    }
                    UnmapViewOfFile(Next.first);
                    if (Next.second != nullptr) CloseHandle(Next.second);
                }
            }).detach();
        }
        void Push(void* View, HANDLE Mapping)
        {
            {
                const std::lock_guard<std::mutex> Hold(QueueLock);
                Queue.emplace_back(View, Mapping);
            }
            Ready.notify_one();
        }

    private:
        std::mutex QueueLock;
        std::condition_variable Ready;
        std::deque<std::pair<void*, HANDLE>> Queue;
    };
    Unmapping& Unmapper()
    {
        static Unmapping* U = new Unmapping();
        return *U;
    }
}

namespace ZoneMemory
{
    void* Allocate(size_t Bytes)
    {
        if (Bytes < SectionFrom) return ::operator new(Bytes);
        const uint64_t Size = Bytes;
        HANDLE M = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, (DWORD)(Size >> 32), (DWORD)Size, nullptr);
        if (M == nullptr) throw std::bad_alloc();
        void* P = MapViewOfFile(M, FILE_MAP_WRITE, 0, 0, Bytes);
        if (P == nullptr)
        {
            CloseHandle(M);
            throw std::bad_alloc();
        }
        const std::lock_guard<std::mutex> Hold(Lock);
        Sections[P] = { M, Bytes };
        return P;
    }

    void Free(void* P, size_t Bytes)
    {
        if (P == nullptr) return;
        if (Bytes < SectionFrom)
        {
            ::operator delete(P);
            return;
        }
        HANDLE M = nullptr;
        {
            const std::lock_guard<std::mutex> Hold(Lock);
            const auto It = Sections.find(P);
            if (It != Sections.end())
            {
                M = It->second.Mapping;
                Sections.erase(It);
            }
        }
        // unmapped on a thread of its own (hundreds of MB of pages torn down: a zone's close waited
        // on it); a copy-on-write view of it keeps the section alive until that view is unmapped
        Unmapper().Push(P, M);
    }

    Copy::Copy(ByteView From)
    {
        if (HANDLE M = SectionOf(From.data(), From.size()); M != nullptr && !From.empty())
            if (void* V = MapViewOfFile(M, FILE_MAP_COPY, 0, 0, From.size()); V != nullptr)
            {
                View = V;
                Data = static_cast<uint8_t*>(V);
                Size = From.size();
                return;
            }
        Owned.assign(From.begin(), From.end());
        Data = Owned.data();
        Size = Owned.size();
    }

    Copy::Copy(std::vector<uint8_t> Bytes) : Owned(std::move(Bytes))
    {
        Data = Owned.data();
        Size = Owned.size();
    }

    Copy::~Copy() { Clear(); }

    Copy::Copy(Copy&& O) noexcept { *this = std::move(O); }

    Copy& Copy::operator=(Copy&& O) noexcept
    {
        if (this == &O) return *this;
        Clear();
        const bool Mine = O.View == nullptr;
        Owned = std::move(O.Owned);
        View = O.View;
        Size = O.Size;
        Data = Mine ? Owned.data() : O.Data;
        O.View = nullptr;
        O.Data = nullptr;
        O.Size = 0;
        return *this;
    }

    void Copy::Clear()
    {
        if (View != nullptr) UnmapViewOfFile(View);
        View = nullptr;
        Owned = {};
        Data = nullptr;
        Size = 0;
    }
}
