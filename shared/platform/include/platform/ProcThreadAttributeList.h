#pragma once

#include <windows.h>

#include <cstddef>
#include <vector>

namespace platform {

// Owns the buffer backing a Windows extended STARTUPINFOEX attribute list.
// Wraps the two-call InitializeProcThreadAttributeList sizing dance.
class ProcThreadAttributeList {
public:
    explicit ProcThreadAttributeList(DWORD attributeCount);

    ProcThreadAttributeList(const ProcThreadAttributeList&) = delete;
    ProcThreadAttributeList& operator=(const ProcThreadAttributeList&) = delete;
    ProcThreadAttributeList(ProcThreadAttributeList&&) = delete;
    ProcThreadAttributeList& operator=(ProcThreadAttributeList&&) = delete;

    ~ProcThreadAttributeList();

    bool Update(DWORD_PTR attribute, void* value, SIZE_T size);

    LPPROC_THREAD_ATTRIBUTE_LIST get() noexcept
    {
        return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer_.data());
    }

private:
    std::vector<std::byte> buffer_;
};

} // namespace platform
