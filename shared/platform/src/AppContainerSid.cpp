#include "platform/AppContainerSid.h"

#include <userenv.h>

#include <stdexcept>
#include <utility>

#pragma comment(lib, "userenv.lib")

namespace platform {

AppContainerSid::AppContainerSid(AppContainerSid&& other) noexcept
    : sid_(std::exchange(other.sid_, nullptr))
{
}

AppContainerSid& AppContainerSid::operator=(AppContainerSid&& other) noexcept
{
    if (this != &other) {
        if (sid_ != nullptr) {
            FreeSid(sid_);
        }
        sid_ = std::exchange(other.sid_, nullptr);
    }
    return *this;
}

AppContainerSid::~AppContainerSid()
{
    if (sid_ != nullptr) {
        FreeSid(sid_);
    }
}

AppContainerSid AppContainerSid::CreateOrOpen(const std::wstring& containerName,
                                               const std::wstring& displayName,
                                               const std::wstring& description)
{
    PSID sid = nullptr;
    HRESULT hr = CreateAppContainerProfile(containerName.c_str(), displayName.c_str(),
                                            description.c_str(), nullptr, 0, &sid);

    if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
        hr = DeriveAppContainerSidFromAppContainerName(containerName.c_str(), &sid);
    }

    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create or derive AppContainer SID");
    }

    return AppContainerSid(sid);
}

void AppContainerSid::Delete(const std::wstring& containerName)
{
    DeleteAppContainerProfile(containerName.c_str());
}

} // namespace platform
