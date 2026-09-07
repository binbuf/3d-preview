#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

// OS shell interop only — no UI. Both features degrade gracefully: an empty
// EnumerateOpenWithHandlers result still gets the synthetic "Choose another
// app..." entry, and ShowWindowsShare returns false with `error` set rather
// than throwing when Share isn't available (e.g. unsupported Windows build).

// One entry in the "Open With" list for the current file — the same
// recommended-handler list Explorer's own Open With submenu shows, via
// SHAssocEnumHandlers, plus a trailing entry that always falls back to the
// system picker (SHOpenWithDialog).
struct OpenWithEntry
{
    std::wstring displayName;
    std::function<bool(const std::wstring& path)> invoke;
};

std::vector<OpenWithEntry> EnumerateOpenWithHandlers(const std::wstring& path);

// Shows the Windows Share flyout (DataTransferManager), anchored to `window`,
// offering `path` as the shared file.
bool ShowWindowsShare(HWND window, const std::wstring& path, std::wstring& error);
