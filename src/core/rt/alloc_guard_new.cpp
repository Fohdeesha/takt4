// Replacement global operator new/delete for the real-time allocation guard
// (core/rt/alloc_guard.hpp). Compiled into executables, not into takt4_core: the
// linker only honours a replacement it finds in an object file it links anyway.
//
// With TAKT4_RT_ALLOC_GUARD off this file is empty and the standard operators stay.

#include "core/rt/alloc_guard.hpp"

#if TAKT4_RT_ALLOC_GUARD

#include <cstddef>
#include <cstdlib>
#include <new>

namespace {

// Runs before main and tells alloc_guard.cpp that the replacements are linked in.
[[maybe_unused]] const bool g_registered = (takt4::rt::detail::markGuardPresent(), true);

void* allocate(std::size_t size) noexcept {
    takt4::rt::detail::checkHeapUse("allocation");
    return std::malloc(size == 0 ? 1 : size);
}

void* allocateAligned(std::size_t size, std::align_val_t alignment) noexcept {
    takt4::rt::detail::checkHeapUse("aligned allocation");
    std::size_t align = static_cast<std::size_t>(alignment);
    if (align < sizeof(void*)) {
        align = sizeof(void*);
    }
    if (size == 0) {
        size = 1;
    }
#if defined(_WIN32)
    return _aligned_malloc(size, align);
#else
    void* p = nullptr;
    if (::posix_memalign(&p, align, size) != 0) {
        return nullptr;
    }
    return p;
#endif
}

void deallocate(void* p) noexcept {
    if (p == nullptr) {
        return;
    }
    takt4::rt::detail::checkHeapUse("deallocation");
    std::free(p);
}

void deallocateAligned(void* p) noexcept {
    if (p == nullptr) {
        return;
    }
    takt4::rt::detail::checkHeapUse("aligned deallocation");
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

} // namespace

// Throwing forms.

void* operator new(std::size_t size) {
    if (void* p = allocate(size)) {
        return p;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    if (void* p = allocate(size)) {
        return p;
    }
    throw std::bad_alloc();
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    if (void* p = allocateAligned(size, alignment)) {
        return p;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    if (void* p = allocateAligned(size, alignment)) {
        return p;
    }
    throw std::bad_alloc();
}

// Nothrow forms.

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return allocate(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return allocateAligned(size, alignment);
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return allocateAligned(size, alignment);
}

// Deallocation, including the sized and nothrow forms so no default falls through
// to the library's own delete (which would not pair with our malloc).

void operator delete(void* p) noexcept {
    deallocate(p);
}

void operator delete[](void* p) noexcept {
    deallocate(p);
}

void operator delete(void* p, std::size_t) noexcept {
    deallocate(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    deallocate(p);
}

void operator delete(void* p, const std::nothrow_t&) noexcept {
    deallocate(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept {
    deallocate(p);
}

void operator delete(void* p, std::align_val_t) noexcept {
    deallocateAligned(p);
}

void operator delete[](void* p, std::align_val_t) noexcept {
    deallocateAligned(p);
}

void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    deallocateAligned(p);
}

void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    deallocateAligned(p);
}

void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    deallocateAligned(p);
}

void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    deallocateAligned(p);
}

#endif // TAKT4_RT_ALLOC_GUARD
