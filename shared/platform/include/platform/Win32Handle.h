#pragma once

#include <windows.h>

#include <utility>

namespace platform {

// Move-only RAII owner for a Win32 HANDLE closed via CloseHandle (process,
// thread, job, and pipe handles all qualify -- their "no handle" sentinel is
// nullptr, not INVALID_HANDLE_VALUE).
class Win32Handle {
public:
    Win32Handle() noexcept = default;
    explicit Win32Handle(HANDLE handle) noexcept : handle_(handle) {}

    Win32Handle(const Win32Handle&) = delete;
    Win32Handle& operator=(const Win32Handle&) = delete;

    Win32Handle(Win32Handle&& other) noexcept : handle_(other.release()) {}

    Win32Handle& operator=(Win32Handle&& other) noexcept
    {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    ~Win32Handle()
    {
        reset();
    }

    HANDLE get() const noexcept { return handle_; }

    explicit operator bool() const noexcept { return handle_ != nullptr; }

    HANDLE release() noexcept
    {
        return std::exchange(handle_, nullptr);
    }

    void reset(HANDLE handle = nullptr) noexcept
    {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

} // namespace platform
