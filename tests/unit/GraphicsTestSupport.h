#pragma once

// One D3D12 device for every [graphics] case in Tests.Unit, plus the adapter
// policy those cases run under.
//
// Five test files previously carried a byte-identical local SharedDevice(),
// each creating its own device. This header replaces all five: the function is
// inline, so the whole binary now shares one device rather than five, and the
// adapter choice is decided in one place.
//
// Why the adapter choice needs a seam at all
// ------------------------------------------
// D3D12Device::Initialize() defaults to a *hardware* adapter and deliberately
// does not fall back to WARP -- ADR-009 rejects automatic WARP fallback,
// because a silent software fallback turns a driver problem into a mysterious
// performance problem in a shipping product. That is the right product policy
// and it is not relaxed here.
//
// It does mean the [graphics] cases cannot create a device at all on a machine
// with no D3D12 hardware adapter, which is every GitHub-hosted CI runner. That
// is a test-environment concern, not a product concern, so the seam lives in
// the test support rather than in D3D12Device: set
//
//     PREVIEW3D_TEST_FORCE_WARP=1
//
// and every device this header hands out is a WARP device. The intent matches
// .docs/design/09-quality-performance-and-security.md:32 -- "A WARP virtual
// machine runs deterministic correctness smoke tests only" -- so a WARP run is
// correctness coverage of the ring/swap-chain/overlay logic and makes no
// performance claim whatsoever. Performance gates are measured on the reference
// hardware in 09-...:11-21 and nowhere else.
//
// A case that genuinely requires a hardware adapter carries the [hardware] tag
// and is excluded from a WARP run.
//
// Current state: THE WARP PATH DOES NOT YET WORK FOR THE WHOLE SUITE.
// Measured on the dev machine (RTX 4080 present, WARP forced via the variable
// above), Debug x64:
//
//   - individual [graphics] cases pass under WARP, including WARP device
//     creation itself and the fence/queue cases;
//   - running the full '[graphics]~[hardware]' set in one process aborts with
//     exit code 125 partway through, taking Catch2's buffered output with it,
//     so no case is even reported as started.
//
// The shape of that -- fine one at a time, fatal in aggregate -- points at
// process-wide state rather than any single case: one shared WARP device is now
// driven through the swap chain, the D3D11On12/D2D bridge and the upload ring in
// the same process. Until it is diagnosed, CI runs Tests.Unit with '~[graphics]'
// (26 of 71 cases) and the [graphics] set runs on a machine with a real adapter.
// Tracked in .docs/TODO.md under Gate 0. Do not "fix" this by relaxing ADR-009.

#include "D3D12Device.h"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

namespace preview3d_test {

// True when PREVIEW3D_TEST_FORCE_WARP is present and is not "0".
inline bool ForceWarpRequested()
{
    static const bool requested = [] {
        wchar_t value[8] = {};
        const DWORD length =
            ::GetEnvironmentVariableW(L"PREVIEW3D_TEST_FORCE_WARP", value, ARRAYSIZE(value));

        // 0 means absent (or an error); a value longer than the buffer is not
        // "0", so treat the truncation case as set.
        if (length == 0) {
            return false;
        }
        if (length >= ARRAYSIZE(value)) {
            return true;
        }
        return !(value[0] == L'0' && value[1] == L'\0');
    }();
    return requested;
}

// The adapter policy every [graphics] case should create devices with, unless
// it is specifically testing adapter selection itself.
inline D3D12Device::CreateOptions DefaultTestDeviceOptions()
{
    D3D12Device::CreateOptions options;
    options.forceWarp = ForceWarpRequested();
    return options;
}

} // namespace preview3d_test

// A process-wide device shared by every [graphics] case. Device creation is
// the expensive part; queues, fences, heaps and resources created against an
// already-created device are cheap.
//
// Deliberately unqualified: the five call sites this replaces used a
// file-local SharedDevice(), and keeping the name unqualified keeps them
// unchanged.
inline D3D12Device& SharedDevice()
{
    static D3D12Device device = [] {
        D3D12Device d;
        auto result = d.Initialize(preview3d_test::DefaultTestDeviceOptions());
        REQUIRE(result.success);
        return d;
    }();
    return device;
}
