// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstddef>
#include <cstdlib>

#include <esp_heap_caps.h>

namespace stackchan::conversation {

// Stateless std-conforming allocator that routes through heap_caps_malloc
// with MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT. Use as a drop-in template
// parameter when you want a std::vector / std::basic_string whose storage
// lives in PSRAM, e.g.
//
//   std::vector<char, PsramAllocator<char>> scratch;
//   scratch.reserve(8192);   // allocation lands in external RAM
//
// ESP-IDF builds with exceptions disabled by default, so allocate() can't
// throw std::bad_alloc — we abort() instead. The container interface
// upstream still believes it will receive an exception, so callers should
// reserve() the worst-case capacity up-front rather than rely on incremental
// growth at runtime when the failure mode matters.
template <typename T>
struct PsramAllocator {
    using value_type = T;

    PsramAllocator() noexcept = default;
    template <typename U>
    PsramAllocator(const PsramAllocator<U>&) noexcept
    {
    }

    T* allocate(std::size_t n)
    {
        if (n == 0) return nullptr;
        void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p == nullptr) {
            // Exceptions are off — there's no std::bad_alloc to throw.
            // Aborting matches ESP-IDF's normal out-of-memory behaviour.
            std::abort();
        }
        return static_cast<T*>(p);
    }

    void deallocate(T* p, std::size_t /*n*/) noexcept
    {
        heap_caps_free(p);
    }
};

template <typename T, typename U>
inline bool operator==(const PsramAllocator<T>&, const PsramAllocator<U>&) noexcept
{
    return true;
}
template <typename T, typename U>
inline bool operator!=(const PsramAllocator<T>&, const PsramAllocator<U>&) noexcept
{
    return false;
}

} // namespace stackchan::conversation
