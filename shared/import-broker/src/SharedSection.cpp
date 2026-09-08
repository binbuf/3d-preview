#include "import_broker/SharedSection.h"

namespace import_broker {

platform::Win32Handle CreateSharedSection(SIZE_T sizeBytes)
{
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                         static_cast<DWORD>(sizeBytes >> 32),
                                         static_cast<DWORD>(sizeBytes & 0xFFFFFFFFu), nullptr);
    if (section == nullptr) {
        return platform::Win32Handle();
    }

    return platform::Win32Handle(section);
}

} // namespace import_broker
