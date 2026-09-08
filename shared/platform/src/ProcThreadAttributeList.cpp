#include "platform/ProcThreadAttributeList.h"

#include <stdexcept>

namespace platform {

ProcThreadAttributeList::ProcThreadAttributeList(DWORD attributeCount)
{
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, attributeCount, 0, &size);
    if (size == 0) {
        throw std::runtime_error("Failed to size process thread attribute list");
    }

    buffer_.resize(size);
    if (!InitializeProcThreadAttributeList(get(), attributeCount, 0, &size)) {
        buffer_.clear();
        throw std::runtime_error("Failed to initialize process thread attribute list");
    }
}

ProcThreadAttributeList::~ProcThreadAttributeList()
{
    if (!buffer_.empty()) {
        DeleteProcThreadAttributeList(get());
    }
}

bool ProcThreadAttributeList::Update(DWORD_PTR attribute, void* value, SIZE_T size)
{
    return UpdateProcThreadAttribute(get(), 0, attribute, value, size, nullptr, nullptr) != FALSE;
}

} // namespace platform
