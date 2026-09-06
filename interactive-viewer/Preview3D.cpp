#include "framework.h"
#include "Preview3D.h"
#include "Model.h"
#include "Renderer.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <cwctype>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr wchar_t kWindowClass[] = L"Preview3DWindow";
constexpr wchar_t kApplicationName[] = L"3D Preview";
constexpr UINT kLoadCompleteMessage = WM_APP + 2;
constexpr UINT_PTR kLoadingTimer = 1;
constexpr UINT_PTR kNavigationTimer = 2;

enum class DragMode { None, Orbit, Pan, Look };

struct CompleteMessage
{
    std::uint64_t generation = 0;
    std::wstring path;
    LoadResult result;
};

struct ViewerApp
{
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND openButton = nullptr;
    HWND fitButton = nullptr;
    HWND resetButton = nullptr;
    HWND menuButton = nullptr;
    HWND retryButton = nullptr;
    HWND openAnotherButton = nullptr;
    HWND copyButton = nullptr;
    HWND tooltip = nullptr;
    HFONT buttonFont = nullptr;
    UINT dpi = 96;
    float dpiScale = 1.0f;
    int toolbarHeight = 52;
    bool rendererReady = false;
    bool closing = false;
    Renderer renderer;
    Camera camera;
    ViewerState state = ViewerState::Empty;
    DragMode dragMode = DragMode::None;
    POINT lastPointer{};
    bool moveForward = false;
    bool moveBackward = false;
    bool moveLeft = false;
    bool moveRight = false;
    bool moveUp = false;
    bool moveDown = false;
    bool rollLeft = false;
    bool rollRight = false;
    ULONGLONG lastNavigationTick = 0;
    std::unordered_map<UINT32, POINT> touchPoints;
    POINT touchCenter{};
    double touchSpan = 0.0;
    std::wstring initialPath;
    std::wstring currentPath;
    std::wstring failedPath;
    std::wstring filename;
    std::wstring status;
    std::wstring readyStatus;
    std::wstring warning;
    std::wstring errorSummary;
    std::wstring errorDetails;
    std::uint64_t generation = 0;
    std::shared_ptr<std::atomic_bool> cancellation;
    std::shared_ptr<std::atomic_bool> alive = std::make_shared<std::atomic_bool>(true);
};

HBRUSH gBackgroundBrush = nullptr;
HWND gMainWindow = nullptr;

int Scale(const ViewerApp& app, int logical)
{
    return MulDiv(logical, static_cast<int>(app.dpi), 96);
}

float ViewportAspect(const ViewerApp& app)
{
    RECT client{};
    GetClientRect(app.window, &client);
    return static_cast<float>(std::max(1L, client.right)) /
        static_cast<float>(std::max(1L, client.bottom - app.toolbarHeight));
}

std::wstring FileNameFromPath(const std::wstring& path)
{
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool HasGlbExtension(const std::wstring& path)
{
    const std::size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring extension = path.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t value)
        { return static_cast<wchar_t>(std::towlower(value)); });
    return extension == L".glb";
}

std::wstring FormatCount(std::uint64_t value)
{
    std::wstring text = std::to_wstring(value);
    for (std::ptrdiff_t index = static_cast<std::ptrdiff_t>(text.size()) - 3; index > 0; index -= 3)
    {
        text.insert(static_cast<std::size_t>(index), 1, L',');
    }
    return text;
}

void UpdateTitle(const ViewerApp& app)
{
    std::wstring title = kApplicationName;
    if (!app.filename.empty()) title = app.filename + L" — " + kApplicationName;
    SetWindowTextW(app.window, title.c_str());
}

bool GetClientPointerPoint(HWND window, UINT32 pointerId, POINT& point)
{
    POINTER_INFO info{};
    if (!GetPointerInfo(pointerId, &info)) return false;
    point = info.ptPixelLocation;
    return ScreenToClient(window, &point) != FALSE;
}

void ResetTouchBaseline(ViewerApp& app)
{
    if (app.touchPoints.empty())
    {
        app.touchCenter = POINT{};
        app.touchSpan = 0.0;
        return;
    }
    auto first = app.touchPoints.begin();
    if (app.touchPoints.size() == 1)
    {
        app.touchCenter = first->second;
        app.touchSpan = 0.0;
        return;
    }
    auto second = std::next(first);
    app.touchCenter = POINT{ (first->second.x + second->second.x) / 2, (first->second.y + second->second.y) / 2 };
    const double x = static_cast<double>(first->second.x - second->second.x);
    const double y = static_cast<double>(first->second.y - second->second.y);
    app.touchSpan = std::sqrt(x * x + y * y);
}

bool CanNavigate(const ViewerApp& app)
{
    return app.state == ViewerState::Ready && app.renderer.HasModel();
}

bool HasNavigationInput(const ViewerApp& app)
{
    return app.moveForward || app.moveBackward || app.moveLeft || app.moveRight ||
        app.moveUp || app.moveDown || app.rollLeft || app.rollRight;
}

void StopNavigation(ViewerApp& app)
{
    app.moveForward = false;
    app.moveBackward = false;
    app.moveLeft = false;
    app.moveRight = false;
    app.moveUp = false;
    app.moveDown = false;
    app.rollLeft = false;
    app.rollRight = false;
    app.lastNavigationTick = 0;
    KillTimer(app.window, kNavigationTimer);
}

bool SetNavigationKey(ViewerApp& app, WPARAM key, bool pressed)
{
    bool* state = nullptr;
    switch (key)
    {
    case 'W': state = &app.moveForward; break;
    case 'S': state = &app.moveBackward; break;
    case 'A': state = &app.moveLeft; break;
    case 'D': state = &app.moveRight; break;
    case 'E': state = &app.moveUp; break;
    case 'Q': state = &app.moveDown; break;
    case 'Z': state = &app.rollLeft; break;
    case 'C': state = &app.rollRight; break;
    default: return false;
    }
    if (*state == pressed) return true;
    *state = pressed;
    if (pressed && CanNavigate(app))
    {
        if (!app.lastNavigationTick) app.lastNavigationTick = GetTickCount64();
        SetTimer(app.window, kNavigationTimer, 16, nullptr);
    }
    else if (!HasNavigationInput(app))
    {
        app.lastNavigationTick = 0;
        KillTimer(app.window, kNavigationTimer);
    }
    return true;
}

void SetControlVisible(HWND control, bool visible)
{
    if (control && ((GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0) != visible)
        ShowWindow(control, visible ? SW_SHOWNA : SW_HIDE);
}

void UpdateButtonAvailability(ViewerApp& app)
{
    const BOOL hasModel = app.renderer.HasModel() ? TRUE : FALSE;
    EnableWindow(app.fitButton, hasModel);
    EnableWindow(app.resetButton, hasModel);
    const bool toolbarVisible = app.state != ViewerState::Loading;
    SetControlVisible(app.openButton, toolbarVisible);
    SetControlVisible(app.fitButton, toolbarVisible);
    SetControlVisible(app.resetButton, toolbarVisible);
    SetControlVisible(app.menuButton, toolbarVisible);
    SetControlVisible(app.retryButton, app.state == ViewerState::Failed);
    SetControlVisible(app.openAnotherButton, app.state == ViewerState::Failed);
    SetControlVisible(app.copyButton, app.state == ViewerState::Failed);
    EnableWindow(app.retryButton, app.failedPath.empty() ? FALSE : TRUE);
}

void LayoutControls(ViewerApp& app)
{
    if (!app.window) return;
    RECT client{};
    GetClientRect(app.window, &client);
    const int margin = Scale(app, 9);
    const int gap = Scale(app, 6);
    const int buttonHeight = Scale(app, 32);
    const int y = (app.toolbarHeight - buttonHeight) / 2;
    const int menuWidth = Scale(app, 38);
    const int resetWidth = Scale(app, 66);
    const int fitWidth = Scale(app, 48);
    const int openWidth = Scale(app, 68);
    int right = client.right - margin;
    MoveWindow(app.menuButton, right - menuWidth, y, menuWidth, buttonHeight, TRUE); right -= menuWidth + gap;
    MoveWindow(app.resetButton, right - resetWidth, y, resetWidth, buttonHeight, TRUE); right -= resetWidth + gap;
    MoveWindow(app.fitButton, right - fitWidth, y, fitWidth, buttonHeight, TRUE); right -= fitWidth + gap;
    MoveWindow(app.openButton, right - openWidth, y, openWidth, buttonHeight, TRUE);

    const RECT card = CalculateErrorCardRect(client.right, client.bottom, app.toolbarHeight, app.dpiScale);
    const int actionHeight = Scale(app, 32);
    const int actionGap = Scale(app, 8);
    const int retryWidth = Scale(app, 78);
    const int anotherWidth = Scale(app, 112);
    const int copyWidth = Scale(app, 104);
    int actionX = card.left + Scale(app, 28);
    const int actionY = card.bottom - Scale(app, 52);
    MoveWindow(app.retryButton, actionX, actionY, retryWidth, actionHeight, TRUE); actionX += retryWidth + actionGap;
    MoveWindow(app.openAnotherButton, actionX, actionY, anotherWidth, actionHeight, TRUE); actionX += anotherWidth + actionGap;
    MoveWindow(app.copyButton, actionX, actionY, copyWidth, actionHeight, TRUE);

}

void RecreateButtonFont(ViewerApp& app)
{
    if (app.buttonFont) DeleteObject(app.buttonFont);
    app.buttonFont = CreateFontW(-Scale(app, 12), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
    for (HWND button : { app.openButton, app.fitButton, app.resetButton, app.menuButton, app.retryButton,
        app.openAnotherButton, app.copyButton })
    {
        if (button) SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(app.buttonFont), TRUE);
    }
}

HWND CreateButton(ViewerApp& app, int id, const wchar_t* text)
{
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 10, 10, app.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app.instance, nullptr);
}

void AddTooltip(ViewerApp& app, HWND control, const wchar_t* text)
{
    TOOLINFOW info{ sizeof(info) };
    info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    info.hwnd = app.window;
    info.uId = reinterpret_cast<UINT_PTR>(control);
    info.lpszText = const_cast<wchar_t*>(text);
    SendMessageW(app.tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
}

void CreateControls(ViewerApp& app)
{
    app.openButton = CreateButton(app, ID_VIEW_OPEN, L"Open");
    app.fitButton = CreateButton(app, ID_VIEW_FIT, L"Fit");
    app.resetButton = CreateButton(app, ID_VIEW_RESET, L"Reset");
    app.menuButton = CreateButton(app, ID_VIEW_MENU, L"•••");
    app.retryButton = CreateButton(app, ID_VIEW_RETRY, L"Retry");
    app.openAnotherButton = CreateButton(app, ID_VIEW_OPEN_ANOTHER, L"Open another");
    app.copyButton = CreateButton(app, ID_VIEW_COPY_DETAILS, L"Copy details");
    app.tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, app.window, nullptr, app.instance, nullptr);
    SetWindowPos(app.tooltip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SendMessageW(app.tooltip, TTM_SETMAXTIPWIDTH, 0, Scale(app, 360));
    AddTooltip(app, app.openButton, L"Open a GLB model (Ctrl+O)");
    AddTooltip(app, app.fitButton, L"Fit the model in the viewport (F)");
    AddTooltip(app, app.resetButton, L"Reset orientation and fit (R)");
    AddTooltip(app, app.menuButton, L"More options and controls (Alt+M)");
    AddTooltip(app, app.retryButton, L"Try opening this file again");
    AddTooltip(app, app.openAnotherButton, L"Choose a different GLB model");
    AddTooltip(app, app.copyButton, L"Copy technical error details without the file path");
    RecreateButtonFont(app);
    UpdateButtonAvailability(app);
    LayoutControls(app);
}

void SetFailure(ViewerApp& app, const std::wstring& summary, const std::wstring& details,
    const std::wstring& failedPath = {})
{
    app.state = ViewerState::Failed;
    app.errorSummary = summary;
    app.errorDetails = details;
    app.failedPath = failedPath;
    app.status.clear();
    KillTimer(app.window, kLoadingTimer);
    StopNavigation(app);
    UpdateButtonAvailability(app);
    LayoutControls(app);
    InvalidateRect(app.window, nullptr, FALSE);
}

void CancelOpen(ViewerApp& app)
{
    if (app.state != ViewerState::Loading) return;
    if (app.cancellation) app.cancellation->store(true, std::memory_order_relaxed);
    ++app.generation;
    app.cancellation.reset();
    KillTimer(app.window, kLoadingTimer);
    app.state = app.renderer.HasModel() ? ViewerState::Ready : ViewerState::Empty;
    if (app.renderer.HasModel()) app.filename = FileNameFromPath(app.currentPath);
    else app.filename.clear();
    app.status = app.renderer.HasModel() ? app.readyStatus : L"";
    UpdateTitle(app);
    UpdateButtonAvailability(app);
    LayoutControls(app);
    InvalidateRect(app.window, nullptr, FALSE);
}

void BeginOpen(ViewerApp& app, const std::wstring& path)
{
    if (path.empty()) return;
    if (app.cancellation)
    {
        app.cancellation->store(true, std::memory_order_relaxed);
        app.cancellation.reset();
        ++app.generation;
    }
    if (path.rfind(L"\\\\", 0) == 0)
    {
        app.filename = FileNameFromPath(path);
        UpdateTitle(app);
        SetFailure(app, L"Remote model paths are not opened.",
            L"Choose a GLB stored on a local drive for this preview slice.", path);
        return;
    }
    if (!HasGlbExtension(path))
    {
        app.filename = FileNameFromPath(path);
        UpdateTitle(app);
        SetFailure(app, L"This format is not included in the current slice.",
            L"Open a self-contained .glb file. Other model formats are deliberately deferred.", path);
        return;
    }

    app.cancellation = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = app.cancellation;
    const auto alive = app.alive;
    const std::uint64_t generation = ++app.generation;
    const HWND window = app.window;

    app.state = ViewerState::Loading;
    StopNavigation(app);
    app.failedPath.clear();
    app.errorSummary.clear();
    app.errorDetails.clear();
    app.filename = FileNameFromPath(path);
    app.status.clear();
    app.warning.clear();
    UpdateTitle(app);
    UpdateButtonAvailability(app);
    LayoutControls(app);
    SetTimer(app.window, kLoadingTimer, 33, nullptr);
    InvalidateRect(app.window, nullptr, FALSE);

    std::thread([window, generation, path, cancellation, alive]()
    {
        LoadResult result;
        try
        {
            result = LoadGlb(path, cancellation, [](const wchar_t*) {});
        }
        catch (const std::bad_alloc&)
        {
            result.summary = L"There is not enough memory to open this model.";
            result.details = L"GLB parsing exceeded the available memory budget.";
        }
        catch (...)
        {
            result.summary = L"This GLB could not be previewed.";
            result.details = L"The importer stopped unexpectedly while reading the model.";
        }
        if (!alive->load(std::memory_order_relaxed)) return;
        auto* message = new (std::nothrow) CompleteMessage{ generation, path, std::move(result) };
        if (message && !PostMessageW(window, kLoadCompleteMessage, 0, reinterpret_cast<LPARAM>(message))) delete message;
    }).detach();
}

void OpenDialog(ViewerApp& app)
{
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    {
        SetFailure(app, L"The Open dialog is unavailable.", L"Windows could not create the system file picker.");
        return;
    }
    const COMDLG_FILTERSPEC filters[] = {
        { L"GLB 3D models (*.glb)", L"*.glb" },
        { L"All files (*.*)", L"*.*" }
    };
    dialog->SetFileTypes(ARRAYSIZE(filters), filters);
    dialog->SetDefaultExtension(L"glb");
    dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    const HRESULT shown = dialog->Show(app.window);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
    if (FAILED(shown))
    {
        SetFailure(app, L"The Open dialog stopped unexpectedly.", L"Try dropping a local .glb file into the window.");
        return;
    }
    ComPtr<IShellItem> item;
    PWSTR selectedPath = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &selectedPath)))
    {
        const std::wstring path(selectedPath);
        CoTaskMemFree(selectedPath);
        BeginOpen(app, path);
    }
}

void CopyErrorDetails(const ViewerApp& app)
{
    std::wstring text = app.errorSummary + L"\r\n\r\n" + app.errorDetails + L"\r\n\r\nFormat: GLB\r\nPhase: opening";
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return;
    void* destination = GlobalLock(memory);
    if (!destination) { GlobalFree(memory); return; }
    std::memcpy(destination, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (OpenClipboard(app.window))
    {
        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        CloseClipboard();
    }
    else
    {
        GlobalFree(memory);
    }
}

void ShowMoreMenu(ViewerApp& app)
{
    HMENU menu = CreatePopupMenu();
    if (!app.warning.empty())
    {
        AppendMenuW(menu, MF_STRING, ID_VIEW_DIAGNOSTICS, L"Model warnings…");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING, ID_VIEW_CONTROLS, L"Controls\t?");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_ABOUT, L"About 3D Preview");
    RECT button{};
    GetWindowRect(app.menuButton, &button);
    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON, button.right, button.bottom, 0, app.window, nullptr);
    DestroyMenu(menu);
}

void ShowControls(HWND owner)
{
    MessageBoxW(owner,
        L"Orbit\tLeft drag or arrow keys\n"
        L"Pan\tMiddle drag, Shift+left drag, or Shift+arrows\n"
        L"Look\tRight drag\n"
        L"Fly\tW/A/S/D\n"
        L"Move vertically\tQ/E\n"
        L"Roll\tZ/C\n"
        L"Move faster\tHold Shift\n"
        L"Zoom\tWheel, +, or −\n"
        L"Fit\tF or double-click\n"
        L"Reset\tR\n"
        L"Open\tCtrl+O\n"
        L"Cancel open\tEsc",
        L"3D Preview controls", MB_OK | MB_ICONINFORMATION);
}

void DrawOwnerButton(ViewerApp& app, const DRAWITEMSTRUCT& item)
{
    wchar_t text[64]{};
    GetWindowTextW(item.hwndItem, text, ARRAYSIZE(text));
    RECT bounds = item.rcItem;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;
    const bool hot = (item.itemState & ODS_HOTLIGHT) != 0;
    const bool primary = item.CtlID == ID_VIEW_OPEN || item.CtlID == ID_VIEW_RETRY;
    COLORREF fill = primary ? RGB(10, 132, 255) : RGB(58, 58, 60);
    COLORREF border = primary ? RGB(34, 146, 255) : RGB(73, 73, 76);
    COLORREF foreground = RGB(245, 245, 247);
    if (hot) fill = primary ? RGB(32, 145, 255) : RGB(68, 68, 71);
    if (pressed) fill = primary ? RGB(0, 113, 227) : RGB(48, 48, 50);
    if (disabled) { fill = RGB(44, 44, 46); border = RGB(51, 51, 53); foreground = RGB(112, 112, 117); }

    const bool errorAction = item.CtlID == ID_VIEW_RETRY || item.CtlID == ID_VIEW_OPEN_ANOTHER ||
        item.CtlID == ID_VIEW_COPY_DETAILS;
    HBRUSH surfaceBrush = CreateSolidBrush(errorAction ? RGB(36, 36, 38) : RGB(44, 44, 46));
    FillRect(item.hDC, &bounds, surfaceBrush);
    DeleteObject(surfaceBrush);

    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, focused ? RGB(100, 188, 255) : border);
    HGDIOBJ oldBrush = SelectObject(item.hDC, brush);
    HGDIOBJ oldPen = SelectObject(item.hDC, pen);
    RoundRect(item.hDC, bounds.left, bounds.top, bounds.right, bounds.bottom, Scale(app, 9), Scale(app, 9));
    SelectObject(item.hDC, oldBrush);
    SelectObject(item.hDC, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);

    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, foreground);
    HGDIOBJ oldFont = SelectObject(item.hDC, app.buttonFont);
    if (pressed) OffsetRect(&bounds, 0, 1);
    DrawTextW(item.hDC, text, -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(item.hDC, oldFont);
}

void RenderFallback(ViewerApp& app, HDC dc)
{
    RECT client{};
    GetClientRect(app.window, &client);
    FillRect(dc, &client, gBackgroundBrush);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(232, 234, 237));
    HFONT old = static_cast<HFONT>(SelectObject(dc, app.buttonFont));
    RECT text = client;
    text.left += Scale(app, 32);
    text.right -= Scale(app, 32);
    DrawTextW(dc, app.errorSummary.c_str(), -1, &text, DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
}

void HandleCommand(ViewerApp& app, int id)
{
    switch (id)
    {
    case ID_VIEW_OPEN:
    case ID_VIEW_OPEN_ANOTHER: OpenDialog(app); break;
    case ID_VIEW_FIT:
        if (app.renderer.HasModel()) { app.camera.Fit(ViewportAspect(app)); InvalidateRect(app.window, nullptr, FALSE); }
        break;
    case ID_VIEW_RESET:
        if (app.renderer.HasModel()) { app.camera.Reset(ViewportAspect(app)); InvalidateRect(app.window, nullptr, FALSE); }
        break;
    case ID_VIEW_MENU: ShowMoreMenu(app); break;
    case ID_VIEW_RETRY: if (!app.failedPath.empty()) BeginOpen(app, app.failedPath); break;
    case ID_VIEW_COPY_DETAILS: CopyErrorDetails(app); break;
    case ID_VIEW_CANCEL: CancelOpen(app); break;
    case ID_VIEW_CONTROLS: ShowControls(app.window); break;
    case ID_VIEW_DIAGNOSTICS:
        MessageBoxW(app.window, app.warning.c_str(), L"Model warnings", MB_OK | MB_ICONWARNING);
        break;
    case IDM_ABOUT:
        MessageBoxW(app.window, L"A focused, native GLB inspection slice.\n\nNo cloud, no editing, no file modification.",
            L"About 3D Preview", MB_OK | MB_ICONINFORMATION);
        break;
    case IDM_EXIT: DestroyWindow(app.window); break;
    }
}

void UpdateNavigation(ViewerApp& app)
{
    const ULONGLONG now = GetTickCount64();
    if (!CanNavigate(app) || !HasNavigationInput(app))
    {
        StopNavigation(app);
        return;
    }

    if (!app.lastNavigationTick)
    {
        app.lastNavigationTick = now;
        return;
    }
    const float elapsed = std::min(0.05f, static_cast<float>(now - app.lastNavigationTick) / 1000.0f);
    app.lastNavigationTick = now;

    float right = (app.moveRight ? 1.0f : 0.0f) - (app.moveLeft ? 1.0f : 0.0f);
    float up = (app.moveUp ? 1.0f : 0.0f) - (app.moveDown ? 1.0f : 0.0f);
    float forward = (app.moveForward ? 1.0f : 0.0f) - (app.moveBackward ? 1.0f : 0.0f);
    const float length = std::sqrt(right * right + up * up + forward * forward);
    if (length > 1.0f)
    {
        right /= length;
        up /= length;
        forward /= length;
    }

    const bool fast = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const float distance = static_cast<float>(app.camera.sceneRadius) * (fast ? 4.0f : 1.0f) * elapsed;
    if (length > 0.0f) app.camera.MoveLocal(right * distance, up * distance, forward * distance);

    const float roll = (app.rollRight ? 1.0f : 0.0f) - (app.rollLeft ? 1.0f : 0.0f);
    if (roll != 0.0f) app.camera.Roll(roll * elapsed * (fast ? 2.4f : 1.2f));
    InvalidateRect(app.window, nullptr, FALSE);
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    ViewerApp* app = reinterpret_cast<ViewerApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        app = static_cast<ViewerApp*>(create->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wParam, lParam);

    switch (message)
    {
    case WM_CREATE:
    {
        app->dpi = GetDpiForWindow(window);
        app->dpiScale = static_cast<float>(app->dpi) / 96.0f;
        app->toolbarHeight = Scale(*app, 52);
        BOOL dark = TRUE;
        DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
        const int cornerPreference = 2;
        DwmSetWindowAttribute(window, 33, &cornerPreference, sizeof(cornerPreference));
        const COLORREF captionColor = RGB(36, 36, 38);
        const COLORREF borderColor = RGB(58, 58, 60);
        DwmSetWindowAttribute(window, 35, &captionColor, sizeof(captionColor));
        DwmSetWindowAttribute(window, 34, &borderColor, sizeof(borderColor));
        DragAcceptFiles(window, TRUE);
        RegisterPointerInputTarget(window, PT_TOUCH);
        RegisterPointerInputTarget(window, PT_PEN);
        CreateControls(*app);
        std::wstring renderError;
        app->rendererReady = app->renderer.Initialize(window, renderError);
        if (!app->rendererReady)
        {
            app->errorSummary = L"Graphics could not be started.";
            app->errorDetails = renderError;
            app->state = ViewerState::Failed;
        }
        UpdateButtonAvailability(*app);
        if (!app->initialPath.empty() && app->rendererReady) BeginOpen(*app, app->initialPath);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_VIEW_OPEN_INITIAL) BeginOpen(*app, app->initialPath);
        else HandleCommand(*app, LOWORD(wParam));
        return 0;
    case WM_DRAWITEM:
        DrawOwnerButton(*app, *reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        if (app->rendererReady)
        {
            OverlayInfo overlay;
            overlay.state = app->state;
            overlay.filename = app->filename;
            overlay.status = app->status;
            overlay.errorSummary = app->errorSummary;
            overlay.errorDetails = app->errorDetails;
            overlay.warning = app->warning;
            overlay.animationPhase = static_cast<float>(GetTickCount64() % 1400) / 1400.0f;
            overlay.dpiScale = app->dpiScale;
            overlay.toolbarHeight = app->toolbarHeight;
            overlay.hasModel = app->renderer.HasModel();
            app->renderer.Render(app->camera, overlay);
        }
        else RenderFallback(*app, dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_SIZE:
        LayoutControls(*app);
        if (app->rendererReady && wParam != SIZE_MINIMIZED)
        {
            std::wstring resizeError;
            if (!app->renderer.Resize(LOWORD(lParam), HIWORD(lParam), resizeError))
            {
                app->rendererReady = false;
                app->errorSummary = L"The viewport could not be resized.";
                app->errorDetails = resizeError;
                app->state = ViewerState::Failed;
            }
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED:
    {
        app->dpi = HIWORD(wParam);
        app->dpiScale = static_cast<float>(app->dpi) / 96.0f;
        app->toolbarHeight = Scale(*app, 52);
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
            suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        RecreateButtonFont(*app);
        LayoutControls(*app);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_GETMINMAXINFO:
    {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = Scale(*app, 480);
        info->ptMinTrackSize.y = Scale(*app, 360);
        return 0;
    }
    case WM_TIMER:
        if (wParam == kLoadingTimer && app->state == ViewerState::Loading) InvalidateRect(window, nullptr, FALSE);
        else if (wParam == kNavigationTimer) UpdateNavigation(*app);
        return 0;
    case WM_DROPFILES:
    {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        if (count != 1)
        {
            DragFinish(drop);
            SetFailure(*app, L"Open one model at a time.", L"Drop exactly one local .glb file into the viewer.");
            return 0;
        }
        const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
        std::wstring path(length + 1, L'\0');
        DragQueryFileW(drop, 0, path.data(), length + 1);
        path.resize(length);
        DragFinish(drop);
        BeginOpen(*app, path);
        return 0;
    }
    case WM_POINTERDOWN:
        if (CanNavigate(*app))
        {
            const UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
            POINT point{};
            if (GetClientPointerPoint(window, pointerId, point) && point.y >= app->toolbarHeight)
            {
                SetFocus(window);
                SetCapture(window);
                app->touchPoints.insert_or_assign(pointerId, point);
                ResetTouchBaseline(*app);
            }
        }
        return 0;
    case WM_POINTERUPDATE:
        if (CanNavigate(*app))
        {
            const UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
            const auto found = app->touchPoints.find(pointerId);
            POINT point{};
            if (found != app->touchPoints.end() && GetClientPointerPoint(window, pointerId, point))
            {
                if (app->touchPoints.size() == 1)
                {
                    app->camera.Orbit(static_cast<float>(point.x - found->second.x), static_cast<float>(point.y - found->second.y));
                    found->second = point;
                    ResetTouchBaseline(*app);
                }
                else
                {
                    found->second = point;
                    auto first = app->touchPoints.begin();
                    auto second = std::next(first);
                    const POINT center{ (first->second.x + second->second.x) / 2, (first->second.y + second->second.y) / 2 };
                    const double spanX = static_cast<double>(first->second.x - second->second.x);
                    const double spanY = static_cast<double>(first->second.y - second->second.y);
                    const double span = std::sqrt(spanX * spanX + spanY * spanY);
                    RECT client{}; GetClientRect(window, &client);
                    app->camera.Pan(static_cast<float>(center.x - app->touchCenter.x), static_cast<float>(center.y - app->touchCenter.y),
                        static_cast<float>(client.bottom - app->toolbarHeight));
                    if (app->touchSpan > 1.0 && span > 1.0)
                    {
                        app->camera.Dolly(static_cast<float>(std::log(span / app->touchSpan) / 0.16));
                    }
                    app->touchCenter = center;
                    app->touchSpan = span;
                }
                InvalidateRect(window, nullptr, FALSE);
            }
        }
        return 0;
    case WM_POINTERUP:
    case WM_POINTERCAPTURECHANGED:
    {
        const UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
        app->touchPoints.erase(pointerId);
        if (app->touchPoints.empty() && GetCapture() == window) ReleaseCapture();
        ResetTouchBaseline(*app);
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (GET_Y_LPARAM(lParam) >= app->toolbarHeight && CanNavigate(*app))
        {
            SetFocus(window);
            SetCapture(window);
            app->dragMode = (GetKeyState(VK_SHIFT) & 0x8000) ? DragMode::Pan : DragMode::Orbit;
            app->lastPointer = POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        }
        return 0;
    case WM_MBUTTONDOWN:
        if (GET_Y_LPARAM(lParam) >= app->toolbarHeight && CanNavigate(*app))
        {
            SetFocus(window);
            SetCapture(window);
            app->dragMode = DragMode::Pan;
            app->lastPointer = POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        }
        return 0;
    case WM_RBUTTONDOWN:
        if (GET_Y_LPARAM(lParam) >= app->toolbarHeight && CanNavigate(*app))
        {
            SetFocus(window);
            SetCapture(window);
            app->dragMode = DragMode::Look;
            app->lastPointer = POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        }
        return 0;
    case WM_MOUSEMOVE:
        if (app->dragMode != DragMode::None && GetCapture() == window)
        {
            const POINT pointer{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const float deltaX = static_cast<float>(pointer.x - app->lastPointer.x);
            const float deltaY = static_cast<float>(pointer.y - app->lastPointer.y);
            app->lastPointer = pointer;
            if (app->dragMode == DragMode::Orbit) app->camera.Orbit(deltaX, deltaY);
            else if (app->dragMode == DragMode::Look) app->camera.Look(deltaX, deltaY);
            else
            {
                RECT client{}; GetClientRect(window, &client);
                app->camera.Pan(deltaX, deltaY, static_cast<float>(client.bottom - app->toolbarHeight));
            }
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
        if (GetCapture() == window) ReleaseCapture();
        app->dragMode = DragMode::None;
        return 0;
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        app->dragMode = DragMode::None;
        return 0;
    case WM_LBUTTONDBLCLK:
        if (GET_Y_LPARAM(lParam) >= app->toolbarHeight && CanNavigate(*app))
        {
            app->camera.Fit(ViewportAspect(*app));
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (CanNavigate(*app))
        {
            app->camera.Dolly(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { CancelOpen(*app); return 0; }
        if (!CanNavigate(*app)) break;
        if (SetNavigationKey(*app, wParam, true)) return 0;
        if (wParam == 'F') app->camera.Fit(ViewportAspect(*app));
        else if (wParam == 'R') app->camera.Reset(ViewportAspect(*app));
        else if (wParam == VK_OEM_PLUS || wParam == VK_ADD) app->camera.Dolly(1.0f);
        else if (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT) app->camera.Dolly(-1.0f);
        else if (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_UP || wParam == VK_DOWN)
        {
            const float x = wParam == VK_LEFT ? -8.0f : (wParam == VK_RIGHT ? 8.0f : 0.0f);
            const float y = wParam == VK_UP ? -8.0f : (wParam == VK_DOWN ? 8.0f : 0.0f);
            if (GetKeyState(VK_SHIFT) & 0x8000)
            {
                RECT client{}; GetClientRect(window, &client);
                app->camera.Pan(x, y, static_cast<float>(client.bottom - app->toolbarHeight));
            }
            else app->camera.Orbit(x, y);
        }
        else break;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_KEYUP:
        if (SetNavigationKey(*app, wParam, false)) return 0;
        break;
    case WM_KILLFOCUS:
        StopNavigation(*app);
        if (GetCapture() == window) ReleaseCapture();
        app->dragMode = DragMode::None;
        return 0;
    case WM_CONTEXTMENU:
        return 0;
    case WM_SYSKEYDOWN:
        if (wParam == 'M' && (lParam & (1u << 29))) { ShowMoreMenu(*app); return 0; }
        break;
    case kLoadCompleteMessage:
    {
        std::unique_ptr<CompleteMessage> complete(reinterpret_cast<CompleteMessage*>(lParam));
        if (!complete || complete->generation != app->generation) return 0;
        app->cancellation.reset();
        KillTimer(window, kLoadingTimer);
        if (complete->result.cancelled)
        {
            app->state = app->renderer.HasModel() ? ViewerState::Ready : ViewerState::Empty;
        }
        else if (!complete->result.succeeded)
        {
            SetFailure(*app, complete->result.summary, complete->result.details, complete->path);
            return 0;
        }
        else
        {
            std::wstring uploadError;
            if (!app->renderer.UploadModel(*complete->result.model, uploadError))
            {
                SetFailure(*app, L"The model was read but could not be displayed.", uploadError, complete->path);
                return 0;
            }
            app->currentPath = complete->path;
            app->filename = FileNameFromPath(complete->path);
            app->warning = complete->result.model->warning;
            app->readyStatus = FormatCount(complete->result.model->triangleCount) +
                L" triangles  •  Right-drag + WASD to fly";
            app->status = app->readyStatus;
            app->camera.SetBounds(complete->result.model->boundsMin, complete->result.model->boundsMax, ViewportAspect(*app));
            app->state = ViewerState::Ready;
            app->failedPath.clear();
            app->errorSummary.clear();
            app->errorDetails.clear();
            UpdateTitle(*app);
            SetFocus(window);
        }
        UpdateButtonAvailability(*app);
        LayoutControls(*app);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_DESTROY:
        app->closing = true;
        app->alive->store(false, std::memory_order_relaxed);
        if (app->cancellation) app->cancellation->store(true, std::memory_order_relaxed);
        KillTimer(window, kLoadingTimer);
        KillTimer(window, kNavigationTimer);
        if (app->buttonFont) { DeleteObject(app->buttonFont); app->buttonFont = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

ATOM RegisterViewerClass(HINSTANCE instance)
{
    WNDCLASSEXW windowClass{ sizeof(windowClass) };
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_PREVIEW3D));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = gBackgroundBrush;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SMALL));
    return RegisterClassExW(&windowClass);
}

bool CreateMainWindow(ViewerApp& app, int showCommand)
{
    const UINT dpi = GetDpiForSystem();
    RECT windowBounds{ 0, 0, MulDiv(1000, dpi, 96), MulDiv(720, dpi, 96) };
    AdjustWindowRectExForDpi(&windowBounds, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_ACCEPTFILES | WS_EX_CONTROLPARENT, dpi);
    const int width = windowBounds.right - windowBounds.left;
    const int height = windowBounds.bottom - windowBounds.top;
    MONITORINFO monitor{ sizeof(monitor) };
    GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const int x = monitor.rcWork.left + std::max<LONG>(0, (monitor.rcWork.right - monitor.rcWork.left - width) / 2);
    const int y = monitor.rcWork.top + std::max<LONG>(0, (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2);
    gMainWindow = CreateWindowExW(WS_EX_ACCEPTFILES | WS_EX_CONTROLPARENT, kWindowClass, kApplicationName,
        WS_OVERLAPPEDWINDOW, x, y, width, height, nullptr, nullptr, app.instance, &app);
    if (!gMainWindow) return false;
    ShowWindow(gMainWindow, showCommand);
    UpdateWindow(gMainWindow);
    return true;
}
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX commonControls{ sizeof(commonControls), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&commonControls);
    gBackgroundBrush = CreateSolidBrush(RGB(28, 28, 30));

    ViewerApp app;
    app.instance = instance;
    int argumentCount = 0;
    PWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments && argumentCount > 1) app.initialPath = arguments[1];
    if (arguments) LocalFree(arguments);

    if (!RegisterViewerClass(instance) || !CreateMainWindow(app, showCommand))
    {
        if (gBackgroundBrush) DeleteObject(gBackgroundBrush);
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 1;
    }

    HACCEL accelerators = LoadAcceleratorsW(instance, MAKEINTRESOURCEW(IDC_PREVIEW3D));
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        if (gMainWindow && IsDialogMessageW(gMainWindow, &message)) continue;
        if (!TranslateAcceleratorW(gMainWindow, accelerators, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    if (gBackgroundBrush) DeleteObject(gBackgroundBrush);
    if (SUCCEEDED(comResult)) CoUninitialize();
    return static_cast<int>(message.wParam);
}
