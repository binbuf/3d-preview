#pragma once

// Shared helper for launching a worker exe (honest or hostile) that speaks
// the StartGeneration/ChunksReady/GenerationError control protocol over a
// pair of pipes plus an inherited shared-section handle. Used by both
// ImportPipelineTests.cpp (the honest worker) and HostileWorkerTests.cpp
// (the synthetic hostile-worker suite).

#include "import_broker/SandboxLauncher.h"
#include "platform/Win32Handle.h"

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

namespace generation_launch_support {

struct GenerationLaunch {
    import_broker::SandboxProcess proc;
    platform::Win32Handle controlInWrite; // host writes StartGenerationRequest here
    platform::Win32Handle controlOutRead; // host reads ChunksReady/GenerationError here
};

// Launches exePath (invoked as `"<exePath>" <args>`) under sid's
// AppContainer sandbox, wiring a request pipe (host->worker, worker's
// stdin) and a notice pipe (worker->host, worker's stdout) plus the given
// shared-section handle, all via the same PROC_THREAD_ATTRIBUTE_HANDLE_LIST
// restricted-inheritance mechanism the launch spike already proved. Does
// NOT resume the process -- callers call import_broker::ResumeSandboxProcess
// explicitly, as a separate, separately-assertable step.
inline std::optional<GenerationLaunch> LaunchWorkerWithControlChannel(
    const std::wstring& exePath, const std::wstring& args, const platform::AppContainerSid& sid,
    HANDLE sectionHandle)
{
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE inReadRaw = nullptr;
    HANDLE inWriteRaw = nullptr;
    if (!CreatePipe(&inReadRaw, &inWriteRaw, &sa, 0)) {
        return std::nullopt;
    }
    platform::Win32Handle controlInRead(inReadRaw);
    platform::Win32Handle controlInWrite(inWriteRaw);
    SetHandleInformation(controlInWrite.get(), HANDLE_FLAG_INHERIT, 0);

    HANDLE outReadRaw = nullptr;
    HANDLE outWriteRaw = nullptr;
    if (!CreatePipe(&outReadRaw, &outWriteRaw, &sa, 0)) {
        return std::nullopt;
    }
    platform::Win32Handle controlOutRead(outReadRaw);
    platform::Win32Handle controlOutWrite(outWriteRaw);
    SetHandleInformation(controlOutRead.get(), HANDLE_FLAG_INHERIT, 0);

    std::vector<HANDLE> inherited{ controlInRead.get(), controlOutWrite.get(), sectionHandle };
    std::wstring cmdLine = L"\"" + exePath + L"\" " + args;

    import_broker::SandboxLimits limits{};
    auto proc = import_broker::LaunchSuspendedSandboxed(exePath, cmdLine, inherited,
                                                          controlOutWrite.get(), limits, sid,
                                                          controlInRead.get());
    if (!proc) {
        return std::nullopt;
    }

    // The parent no longer needs its own copies of the ends it handed to
    // the child.
    controlInRead.reset();
    controlOutWrite.reset();

    GenerationLaunch launch;
    launch.proc = std::move(*proc);
    launch.controlInWrite = std::move(controlInWrite);
    launch.controlOutRead = std::move(controlOutRead);
    return launch;
}

} // namespace generation_launch_support
