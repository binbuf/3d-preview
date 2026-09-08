#pragma once

#include "platform/Win32Handle.h"

#include <windows.h>

namespace import_broker {

// Generously larger than the ~1.5 KiB synthetic cube+point-cluster payload;
// bounded and arbitrary but documented. Revisit against real budgets in
// Gate 3+.
constexpr SIZE_T kSyntheticSectionBytes = 1ULL * 1024 * 1024;

// Creates a pagefile-backed, PAGE_READWRITE shared section of exactly
// sizeBytes (hFile = INVALID_HANDLE_VALUE -- system paging file, per
// .docs/design/03-file-formats-and-ingestion.md: "bounded scratch section,
// not a real file"). The returned handle has
// SECURITY_ATTRIBUTES.bInheritHandle = TRUE baked in at creation, so it is
// eligible for PROC_THREAD_ATTRIBUTE_HANDLE_LIST inheritance the same way
// the launch spike's pipes already are. No explicit AppContainer ACE is set
// on the security descriptor -- Windows' access check happens at handle
// open/duplicate time, not on every use of an already-inherited handle, the
// same reason the existing pipes already work with a null security
// descriptor. (Flagged as a risk to confirm empirically the first time a
// worker actually maps an inherited section under a real AppContainer
// token.)
platform::Win32Handle CreateSharedSection(SIZE_T sizeBytes);

} // namespace import_broker
