#pragma once

#include <windows.h>

#include <string>

namespace platform {

// Move-only RAII owner of an AppContainer profile SID.
class AppContainerSid {
public:
    AppContainerSid() noexcept = default;

    AppContainerSid(const AppContainerSid&) = delete;
    AppContainerSid& operator=(const AppContainerSid&) = delete;

    AppContainerSid(AppContainerSid&& other) noexcept;
    AppContainerSid& operator=(AppContainerSid&& other) noexcept;

    ~AppContainerSid();

    // Creates the named AppContainer profile and returns its SID. Process
    // creation under an AppContainer token expects the profile to exist on
    // disk/in the registry, which is why this calls CreateAppContainerProfile
    // rather than the side-effect-free DeriveAppContainerSidFromAppContainerName
    // -- that call is used here only as the ERROR_ALREADY_EXISTS fallback, to
    // recover the same SID when a prior run's profile was not cleaned up.
    static AppContainerSid CreateOrOpen(const std::wstring& containerName,
                                         const std::wstring& displayName,
                                         const std::wstring& description);

    // Removes the profile created by CreateOrOpen. Call in test teardown so
    // repeated runs don't accumulate entries under %LOCALAPPDATA%\Packages.
    static void Delete(const std::wstring& containerName);

    PSID get() const noexcept { return sid_; }
    explicit operator bool() const noexcept { return sid_ != nullptr; }

private:
    explicit AppContainerSid(PSID sid) noexcept
        : sid_(sid)
    {
    }

    PSID sid_ = nullptr;
};

} // namespace platform
