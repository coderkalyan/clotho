// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// The operating-system services the runtime needs beyond the C library: aligned
// allocation, aligned mappings of address space, and the one thread, mutex and
// condition variables the VCD writer
// uses. They are written against POSIX and Win32 directly rather than through
// std::thread, std::mutex and aligned operator new because those live in the C++
// library's compiled part, and an executable's runtime links nothing but the C
// library and compiler-rt (docs/design/executables.md). Every object here is
// constant-initialized, so a namespace-scope one runs no constructor.

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <malloc.h>
#include <windows.h>
#else
#include <pthread.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace clotho::rt {

// `size` bytes aligned to `align` (a power of two, at least sizeof(void*)), or
// null. Released only by os_aligned_free.
inline void* os_aligned_alloc(size_t size, size_t align) {
#if defined(_WIN32)
    // The MSVC runtime has no aligned_alloc, and _aligned_malloc memory must go
    // back through _aligned_free.
    return _aligned_malloc(size, align);
#else
    void* p = nullptr;
    return posix_memalign(&p, align, size) == 0 ? p : nullptr;
#endif
}

inline void os_aligned_free(void* p) {
#if defined(_WIN32)
    _aligned_free(p);
#else
    free(p);
#endif
}

// A mapping of `size` bytes of zeroed memory whose base is a multiple of `align` (a
// power of two, a multiple of the page size), or null. It is the caller's alone:
// nothing else is placed in it, it covers `size` rounded up to whole pages and no
// more, and os_unmap with the same `size` gives all of it back to the system. This
// is what the collector's chunks and large objects are, rather than allocations:
// an allocator asked for a 4 MiB alignment reserves far more than it hands out
// and keeps what it is given back.
inline void* os_map_aligned(size_t size, size_t align) {
#if defined(_WIN32)
    // A reservation cannot be released in part here, so one large enough to hold an
    // aligned base is made to find such a base, released, and made again at exactly
    // that address. Another thread may take the address in between, hence the
    // bounded retry; running out of tries reports as running out of memory.
    for (int attempt = 0; attempt < 8; ++attempt) {
        void* const probe = VirtualAlloc(nullptr, size + align, MEM_RESERVE, PAGE_NOACCESS);
        if (probe == nullptr) {
            return nullptr;
        }
        const uintptr_t base = (reinterpret_cast<uintptr_t>(probe) + align - 1) & ~(static_cast<uintptr_t>(align) - 1);
        VirtualFree(probe, 0, MEM_RELEASE);
        void* const p = VirtualAlloc(reinterpret_cast<void*>(base), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p != nullptr) {
            return p;
        }
    }
    return nullptr;
#else
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    const size_t len  = (size + page - 1) & ~(page - 1);
    // Map enough that an aligned base lies inside, then give back what is before
    // it and what is after the mapping proper.
    void* const raw = mmap(nullptr, len + align, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) {
        return nullptr;
    }
    const uintptr_t lo   = reinterpret_cast<uintptr_t>(raw);
    const uintptr_t base = (lo + align - 1) & ~(static_cast<uintptr_t>(align) - 1);
    if (base != lo) {
        munmap(raw, base - lo);
    }
    const uintptr_t end = lo + len + align;
    if (end != base + len) {
        munmap(reinterpret_cast<void*>(base + len), end - (base + len));
    }
    return reinterpret_cast<void*>(base);
#endif
}

// Give back a mapping os_map_aligned made, with the `size` it was made with.
inline void os_unmap(void* p, size_t size) {
#if defined(_WIN32)
    (void)size;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    munmap(p, (size + page - 1) & ~(page - 1));
#endif
}

class OsMutex {
public:
    void lock() {
#if defined(_WIN32)
        AcquireSRWLockExclusive(&lock_);
#else
        pthread_mutex_lock(&lock_);
#endif
    }

    void unlock() {
#if defined(_WIN32)
        ReleaseSRWLockExclusive(&lock_);
#else
        pthread_mutex_unlock(&lock_);
#endif
    }

private:
    friend class OsCond;
#if defined(_WIN32)
    SRWLOCK lock_ = SRWLOCK_INIT;
#else
    pthread_mutex_t lock_ = PTHREAD_MUTEX_INITIALIZER;
#endif
};

// Holds `mutex` for the enclosing scope.
class OsLock {
public:
    explicit OsLock(OsMutex& mutex)
      : mutex_(mutex) {
        mutex_.lock();
    }
    ~OsLock() {
        mutex_.unlock();
    }
    OsLock(const OsLock&)            = delete;
    OsLock& operator=(const OsLock&) = delete;

    OsMutex& mutex() {
        return mutex_;
    }

private:
    OsMutex& mutex_;
};

class OsCond {
public:
    // Block until `ready()` holds, releasing the held lock while asleep.
    template<class Pred>
    void wait(OsLock& held, Pred ready) {
        while (!ready()) {
#if defined(_WIN32)
            SleepConditionVariableSRW(&cond_, &held.mutex().lock_, INFINITE, 0);
#else
            pthread_cond_wait(&cond_, &held.mutex().lock_);
#endif
        }
    }

    void notify_one() {
#if defined(_WIN32)
        WakeConditionVariable(&cond_);
#else
        pthread_cond_signal(&cond_);
#endif
    }

    void notify_all() {
#if defined(_WIN32)
        WakeAllConditionVariable(&cond_);
#else
        pthread_cond_broadcast(&cond_);
#endif
    }

private:
#if defined(_WIN32)
    CONDITION_VARIABLE cond_ = CONDITION_VARIABLE_INIT;
#else
    pthread_cond_t cond_ = PTHREAD_COND_INITIALIZER;
#endif
};

class OsThread {
public:
    // Run `body` on a new thread. False if the OS refused to create one.
    bool start(void (*body)()) {
#if defined(_WIN32)
        handle_  = CreateThread(nullptr, 0, &trampoline, reinterpret_cast<void*>(body), 0, nullptr);
        running_ = handle_ != nullptr;
#else
        running_ = pthread_create(&handle_, nullptr, &trampoline, reinterpret_cast<void*>(body)) == 0;
#endif
        return running_;
    }

    bool joinable() const {
        return running_;
    }

    void join() {
#if defined(_WIN32)
        WaitForSingleObject(handle_, INFINITE);
        CloseHandle(handle_);
#else
        pthread_join(handle_, nullptr);
#endif
        running_ = false;
    }

private:
#if defined(_WIN32)
    static DWORD WINAPI trampoline(void* body) {
        reinterpret_cast<void (*)()>(body)();
        return 0;
    }
    HANDLE handle_ = nullptr;
#else
    static void* trampoline(void* body) {
        reinterpret_cast<void (*)()>(body)();
        return nullptr;
    }
    pthread_t handle_{};
#endif
    bool running_ = false;
};

} // namespace clotho::rt
