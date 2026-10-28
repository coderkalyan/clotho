// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// The operating-system services the runtime needs beyond the C library: aligned
// allocation, and the one thread, mutex and condition variables the VCD writer
// uses. They are written against POSIX and Win32 directly rather than through
// std::thread, std::mutex and aligned operator new because those live in the C++
// library's compiled part, and an executable's runtime links nothing but the C
// library and compiler-rt (docs/design/executables.md). Every object here is
// constant-initialized, so a namespace-scope one runs no constructor.

#include <stddef.h>

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
