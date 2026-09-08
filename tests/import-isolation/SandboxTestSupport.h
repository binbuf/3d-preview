#pragma once

// Shared test fixtures for launching the real Preview3DImportWorker.exe
// under a throwaway AppContainer profile, used by both SandboxLaunchTests.cpp
// (the AppContainer/Job Object launch spike) and ImportPipelineTests.cpp
// (the wire-format/broker-protocol data path).

#include "platform/AppContainerSid.h"

#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <string>

#ifndef PREVIEW3D_IMPORT_WORKER_EXE
#error "PREVIEW3D_IMPORT_WORKER_EXE must be defined by Tests.ImportIsolation.vcxproj"
#endif

namespace sandbox_test_support {

inline const wchar_t* WorkerExePath()
{
    return PREVIEW3D_IMPORT_WORKER_EXE;
}

// Grants ALL APPLICATION PACKAGES / ALL RESTRICTED APPLICATION PACKAGES
// read+execute on the worker's build output directory. AppContainer
// processes are checked against these SIDs to load even their own .exe, and
// a normal dev/CI build output folder has no such ACE by default -- without
// this, every launch in this file fails at the loader level with
// ERROR_ACCESS_DENIED before any of our own logic runs. Idempotent, so it
// is safe to call once per test run.
inline void GrantAppContainerAccessToWorkerDirectory()
{
    std::wstring exePath(WorkerExePath());
    auto lastSlash = exePath.find_last_of(L"\\/");
    std::wstring directory = (lastSlash == std::wstring::npos) ? L"." : exePath.substr(0, lastSlash);

    std::wstring command = L"icacls \"" + directory
        + L"\" /grant *S-1-15-2-1:(OI)(CI)RX /grant *S-1-15-2-2:(OI)(CI)RX /Q";
    _wsystem(command.c_str());
}

inline std::wstring MakeUniqueContainerName()
{
    auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return L"Preview3DSandboxSpike-" + std::to_wstring(GetCurrentProcessId()) + L"-"
        + std::to_wstring(ticks);
}

// Owns a throwaway AppContainer profile for the lifetime of one test case,
// so repeated runs don't accumulate entries under %LOCALAPPDATA%\Packages.
struct SandboxFixture {
    std::wstring containerName;
    platform::AppContainerSid sid;

    SandboxFixture()
        : containerName(MakeUniqueContainerName())
        , sid(platform::AppContainerSid::CreateOrOpen(
              containerName, L"Preview3D Sandbox Spike",
              L"Test-only AppContainer profile for Gate 2 workstream A"))
    {
        GrantAppContainerAccessToWorkerDirectory();
    }

    ~SandboxFixture()
    {
        platform::AppContainerSid::Delete(containerName);
    }
};

} // namespace sandbox_test_support
