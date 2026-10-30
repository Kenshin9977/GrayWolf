#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

// A vector of trivially copyable elements whose first N live inside it: no heap allocation until
// it outgrows them (a vertex's few weights and UV layers, millions of vertices per export). The
// subset of std::vector's interface the model code uses.
template <typename T, size_t N>
class InlineVector
{
    static_assert(std::is_trivially_copyable_v<T> && N > 0);

public:
    using value_type = T;
    using size_type = size_t;
    using iterator = T*;
    using const_iterator = const T*;
    using reference = T&;
    using const_reference = const T&;

    InlineVector() noexcept = default;
    InlineVector(const InlineVector& Other) { Append(Other.data(), Other.Count); }
    InlineVector(InlineVector&& Other) noexcept { Take(Other); }
    InlineVector& operator=(const InlineVector& Other)
    {
        if (this != &Other)
        {
            Count = 0;
            Append(Other.data(), Other.Count);
        }
        return *this;
    }
    InlineVector& operator=(InlineVector&& Other) noexcept
    {
        if (this != &Other)
        {
            Free();
            Take(Other);
        }
        return *this;
    }
    ~InlineVector() { Free(); }

    size_t size() const noexcept { return Count; }
    size_t capacity() const noexcept { return Capacity; }
    bool empty() const noexcept { return Count == 0; }

    T* data() noexcept { return Heap ? Heap : reinterpret_cast<T*>(Local); }
    const T* data() const noexcept { return Heap ? Heap : reinterpret_cast<const T*>(Local); }
    T* begin() noexcept { return data(); }
    T* end() noexcept { return data() + Count; }
    const T* begin() const noexcept { return data(); }
    const T* end() const noexcept { return data() + Count; }
    T& operator[](size_t I) noexcept { return data()[I]; }
    const T& operator[](size_t I) const noexcept { return data()[I]; }
    T& front() noexcept { return data()[0]; }
    const T& front() const noexcept { return data()[0]; }
    T& back() noexcept { return data()[Count - 1]; }
    const T& back() const noexcept { return data()[Count - 1]; }

    void reserve(size_t Want)
    {
        if (Want > Capacity) Grow(Want);
    }
    void clear() noexcept { Count = 0; }

    template <typename... Args>
    T& emplace_back(Args&&... A)
    {
        // built before a growth that may move the storage an argument refers to
        T Value(std::forward<Args>(A)...);
        if (Count == Capacity) Grow(Capacity + (Capacity / 2) + 1);
        T* Slot = ::new (static_cast<void*>(data() + Count)) T(Value);
        Count++;
        return *Slot;
    }
    void push_back(const T& Value) { emplace_back(Value); }

private:
    void Append(const T* From, size_t Size)
    {
        reserve(Count + Size);
        if (Size) std::memcpy(static_cast<void*>(data() + Count), From, Size * sizeof(T));
        Count += (uint32_t)Size;
    }
    void Grow(size_t Want)
    {
        T* Bigger = std::allocator<T>().allocate(Want);
        if (Count) std::memcpy(static_cast<void*>(Bigger), data(), Count * sizeof(T));
        Free();
        Heap = Bigger;
        Capacity = (uint32_t)Want;
    }
    void Free() noexcept
    {
        if (Heap) std::allocator<T>().deallocate(Heap, Capacity);
        Heap = nullptr;
        Capacity = (uint32_t)N;
    }
    void Take(InlineVector& Other) noexcept
    {
        if (Other.Heap)
        {
            Heap = std::exchange(Other.Heap, nullptr);
            Capacity = std::exchange(Other.Capacity, (uint32_t)N);
        }
        else if (Other.Count)
            std::memcpy(Local, Other.Local, Other.Count * sizeof(T));
        Count = std::exchange(Other.Count, 0);
    }

    T* Heap = nullptr;   // the elements once past N, else null (they are in Local)
    uint32_t Count = 0;
    uint32_t Capacity = (uint32_t)N;
    alignas(T) unsigned char Local[N * sizeof(T)]{};
};
