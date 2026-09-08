#pragma once

#include <windows.h>

#include <cstddef>
#include <span>

namespace platform {

// Move-only RAII wrapper around MapViewOfFile/UnmapViewOfFile. Plain
// MapViewOfFile, not MapViewOfFile3 -- this project has no placeholder/NUMA
// mapping need. No FlushViewOfFile: the sections this wraps are
// pagefile-backed, not file-backed, and cross-process visibility of a live
// shared section is automatic (coherent shared physical pages); ordering
// between writer and reader is guaranteed by the control-channel
// WriteFile/ReadFile syscalls themselves, not by this class.
class MappedView {
public:
    MappedView() noexcept = default;

    MappedView(const MappedView&) = delete;
    MappedView& operator=(const MappedView&) = delete;

    MappedView(MappedView&& other) noexcept;
    MappedView& operator=(MappedView&& other) noexcept;

    ~MappedView();

    // Maps MapViewOfFile(section, desiredAccess, 0, 0, sizeBytes). sizeBytes
    // == 0 maps the whole section. Returns an empty (operator bool false)
    // view on failure.
    static MappedView Map(HANDLE section, DWORD desiredAccess, SIZE_T sizeBytes);

    std::span<std::byte> bytes() noexcept;
    std::span<const std::byte> bytes() const noexcept;

    explicit operator bool() const noexcept { return view_ != nullptr; }

private:
    MappedView(void* view, SIZE_T size) noexcept
        : view_(view)
        , size_(size)
    {
    }

    void Reset() noexcept;

    void* view_ = nullptr;
    SIZE_T size_ = 0;
};

} // namespace platform
