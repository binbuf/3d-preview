#include "framework.h"
#include "Preview3D.h"
#include "Model.h"
#include "Renderer.h"
#include "NavGizmo.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <cwctype>
#include <iomanip>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr wchar_t kWindowClass[] = L"Preview3DWindow";
constexpr wchar_t kApplicationName[] = L"3D Preview";
constexpr UINT kLoadCompleteMessage = WM_APP + 2;
constexpr float kArrowPixelsPerSecond = 340.0f;
constexpr double kHudVisibleSeconds = 1.3;
constexpr double kHudFadeSeconds = 0.30;
constexpr double kToggleRepeatGuardSeconds = 0.30;
constexpr double kClickMaxSeconds = 0.5;
constexpr float kClickDragThresholdPixels = 6.0f;
// Caps how many queued messages the main loop drains before it is forced to
// re-check animation state and render, so any burst or self-sustaining
// message flood can never fully starve rendering.
constexpr int kMaxDrainedMessagesPerIteration = 32;

// Exclusive pointer state machine. Exactly one mode owns camera/selection
// input at a time; the mode is chosen at button-down and stays locked for the
// whole gesture, so mid-drag modifier changes never switch modes.
//
//   None        idle; gizmo hover tracking only
//   Orbit       LMB or MMB held on the canvas: orbit-drags the camera. An LMB
//               gesture that stays under the click/drag threshold also
//               click-selects on release (drag and click share LMB).
//   GizmoOrbit  LMB held on the gizmo ball: wrapped orbit drag (same math as
//               Orbit, kept distinct only because it started on the gizmo).
//   (Axis nodes/stems snap on press, so they never enter a drag mode.)
//   FlyLook     RMB held: Unreal-style capture. Cursor is hidden and
//               recentered each move; WASD/Q/E fly, wheel adjusts speed.
//   Truck       MMB held: trucks along the world ground plane (flattened
//               forward/right), optionally snapped to the nearest world axis.
//   DollyDrag   Ctrl+MMB: smooth exponential dolly on vertical drag.
enum class PointerMode
{
    None,
    GizmoOrbit,
    FlyLook,
    Orbit,
    Truck,
    DollyDrag
};

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
    HWND gridButton = nullptr;
    HWND axisSnapButton = nullptr;
    HWND speedSlider = nullptr;
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
    NavGizmo gizmo;
    ViewerState state = ViewerState::Empty;
    PointerMode pointerMode = PointerMode::None;
    POINT lastPointer{};
    bool flyLook = false;               // RMB capture active
    POINT flyPressPoint{};              // screen point to restore the cursor to
    bool wrapDrag = false;              // cursor wrapping (infinite drag) active
    bool selectDragged = false;
    POINT selectDownPoint{};
    double selectDownSeconds = 0.0;
    double orbitVelocityX = 0.0;
    double orbitVelocityY = 0.0;
    double lastOrbitMoveSeconds = 0.0;
    double panVelocityX = 0.0;
    double panVelocityY = 0.0;
    double lastPanMoveSeconds = 0.0;
    bool moveForward = false;
    bool moveBackward = false;
    bool moveLeft = false;
    bool moveRight = false;
    bool moveUp = false;
    bool moveDown = false;
    bool rollLeft = false;
    bool rollRight = false;
    bool arrowLeft = false;
    bool arrowRight = false;
    bool arrowUp = false;
    bool arrowDown = false;
    double lastFrameSeconds = 0.0;
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
    std::shared_ptr<ModelData> loadedModel;  // CPU copy kept alive for picking
    bool meshSelected = false;
    bool gridVisible = true;
    bool axisSnapEnabled = false;
    std::wstring speedHudText;
    double speedHudUntil = 0.0;
    std::wstring modeHudText;
    double modeHudUntil = 0.0;
    int lastToggleId = 0;
    double lastToggleSeconds = 0.0;
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

double NowSeconds()
{
    static const double frequency = []() -> double
    {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / frequency;
}float ViewportAspect(const ViewerApp& app)
{
    RECT client{};
    GetClientRect(app.window, &client);
    return static_cast<float>(std::max(1L, client.right)) /
        static_cast<float>(std::max(1L, client.bottom - app.toolbarHeight));
}

// The 3D viewport is the client area below the toolbar.
RECT ViewportRect(const ViewerApp& app)
{
    RECT viewport{};
    GetClientRect(app.window, &viewport);
    viewport.top = app.toolbarHeight;
    return viewport;
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

std::wstring FormatMultiplier(double value)
{
    std::wostringstream text;
    text << std::fixed << std::setprecision(value >= 10.0 ? 1 : 2) << value;
    return text.str();
}

double HudAlpha(double visibleUntil, double now)
{
    const double remaining = visibleUntil - now;
    if (remaining <= 0.0) return 0.0;
    return static_cast<double>(std::clamp(remaining / kHudFadeSeconds, 0.0, 1.0));
}

void ShowSpeedHud(ViewerApp& app, const std::wstring& text)
{
    app.speedHudText = text;
    app.speedHudUntil = NowSeconds() + kHudVisibleSeconds;
    InvalidateRect(app.window, nullptr, FALSE);
}

void ShowModeHud(ViewerApp& app, const std::wstring& text)
{
    app.modeHudText = text;
    app.modeHudUntil = NowSeconds() + kHudVisibleSeconds;
    InvalidateRect(app.window, nullptr, FALSE);
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
    // Flight keys only count as motion while RMB capture is active.
    if (app.flyLook && (app.moveForward || app.moveBackward || app.moveLeft || app.moveRight ||
        app.moveUp || app.moveDown || app.rollLeft || app.rollRight)) return true;
    return app.arrowLeft || app.arrowRight || app.arrowUp || app.arrowDown;
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
    app.arrowLeft = false;
    app.arrowRight = false;
    app.arrowUp = false;
    app.arrowDown = false;
    app.camera.StopMotion();
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
    case VK_LEFT: state = &app.arrowLeft; break;
    case VK_RIGHT: state = &app.arrowRight; break;
    case VK_UP: state = &app.arrowUp; break;
    case VK_DOWN: state = &app.arrowDown; break;
    default: return false;
    }
    *state = pressed;
    return true;
}

void UpdateGizmoLayout(ViewerApp& app)
{
    RECT client{};
    GetClientRect(app.window, &client);
    app.gizmo.UpdateLayout(client.right, client.bottom, app.toolbarHeight, app.dpiScale);
}

void BeginWrappedDrag(ViewerApp& app, const POINT& point)
{
    app.wrapDrag = true;
    app.lastPointer = point;
    // Keep the cursor inside the viewport during the drag so wrapping can
    // always teleport before a display edge traps the gesture.
    RECT clip = ViewportRect(app);
    MapWindowPoints(app.window, nullptr, reinterpret_cast<LPPOINT>(&clip), 2);
    ClipCursor(&clip);
}

void WrapCursorIfNeeded(ViewerApp& app)
{
    if (!app.wrapDrag) return;
    const RECT viewport = ViewportRect(app);
    const LONG margin = std::max<LONG>(2, Scale(app, 10));
    const LONG width = viewport.right - viewport.left;
    const LONG height = viewport.bottom - viewport.top;
    POINT point = app.lastPointer;
    LONG shiftX = 0;
    LONG shiftY = 0;
    if (width > margin * 2 + 2)
    {
        if (point.x <= viewport.left + margin) shiftX = width - margin * 2;
        else if (point.x >= viewport.right - margin) shiftX = -(width - margin * 2);
    }
    if (height > margin * 2 + 2)
    {
        if (point.y <= viewport.top + margin) shiftY = height - margin * 2;
        else if (point.y >= viewport.bottom - margin) shiftY = -(height - margin * 2);
    }
    if (shiftX == 0 && shiftY == 0) return;
    point.x += shiftX;
    point.y += shiftY;
    POINT screen = point;
    ClientToScreen(app.window, &screen);
    SetCursorPos(screen.x, screen.y);
    // The teleport posts one synthetic WM_MOUSEMOVE that lands exactly on
    // this point, so seeding lastPointer here keeps its delta at zero and the
    // drag continues seamlessly from the far side.
    app.lastPointer = point;
}

void EndPointer(ViewerApp& app)
{
    if (app.wrapDrag)
    {
        app.wrapDrag = false;
        ClipCursor(nullptr);
    }
    if (app.flyLook)
    {
        app.flyLook = false;
        SetCursorPos(app.flyPressPoint.x, app.flyPressPoint.y);
    }
    if (GetCapture() == app.window) ReleaseCapture();
    app.pointerMode = PointerMode::None;
}

void TrackOrbitVelocity(ViewerApp& app, float deltaX, float deltaY)
{
    const double now = NowSeconds();
    const double gap = now - app.lastOrbitMoveSeconds;
    if (gap > 0.0005 && gap < 0.15)
    {
        app.orbitVelocityX = app.orbitVelocityX * 0.62 + (deltaX / gap) * 0.38;
        app.orbitVelocityY = app.orbitVelocityY * 0.62 + (deltaY / gap) * 0.38;
    }
    else
    {
        app.orbitVelocityX = 0.0;
        app.orbitVelocityY = 0.0;
    }
    app.lastOrbitMoveSeconds = now;
}

void TrackPanVelocity(ViewerApp& app, float deltaX, float deltaY)
{
    const double now = NowSeconds();
    const double gap = now - app.lastPanMoveSeconds;
    if (gap > 0.0005 && gap < 0.15)
    {
        app.panVelocityX = app.panVelocityX * 0.62 + (deltaX / gap) * 0.38;
        app.panVelocityY = app.panVelocityY * 0.62 + (deltaY / gap) * 0.38;
    }
    else
    {
        app.panVelocityX = 0.0;
        app.panVelocityY = 0.0;
    }
    app.lastPanMoveSeconds = now;
}

void BuildPickRay(const Camera& camera, float pointerX, float pointerY,
    float viewportWidth, float viewportHeight, DirectX::XMVECTOR& origin, DirectX::XMVECTOR& direction)
{
    using namespace DirectX;
    const float width = std::max(1.0f, viewportWidth);
    const float height = std::max(1.0f, viewportHeight);
    const float ndcX = 2.0f * (pointerX / width) - 1.0f;
    const float ndcY = 1.0f - 2.0f * (pointerY / height);
    const XMVECTOR orientation = camera.Orientation();
    const float tanHalf = std::tan(kVerticalFieldOfView * 0.5f);
    if (camera.Projection() == ProjectionMode::Orthographic)
    {
        const float halfHeight = static_cast<float>(camera.distance) * tanHalf;
        const float halfWidth = halfHeight * (width / height);
        origin = camera.EyePosition() + XMVector3Rotate(
            XMVectorSet(ndcX * halfWidth, ndcY * halfHeight, 0.0f, 0.0f), orientation);
        direction = XMVector3Rotate(XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f), orientation);
    }
    else
    {
        origin = camera.EyePosition();
        direction = XMVector3Rotate(XMVector3Normalize(
            XMVectorSet(ndcX * tanHalf * (width / height), ndcY * tanHalf, -1.0f, 0.0f)), orientation);
    }
}

void ClickSelect(ViewerApp& app, const POINT& point)
{
    if (!app.loadedModel) return;
    const RECT viewport = ViewportRect(app);
    DirectX::XMVECTOR origin{};
    DirectX::XMVECTOR direction{};
    BuildPickRay(app.camera, static_cast<float>(point.x), static_cast<float>(point.y),
        static_cast<float>(viewport.right - viewport.left),
        static_cast<float>(viewport.bottom - viewport.top), origin, direction);
    DirectX::XMFLOAT3 originValue{};
    DirectX::XMFLOAT3 directionValue{};
    DirectX::XMStoreFloat3(&originValue, origin);
    DirectX::XMStoreFloat3(&directionValue, direction);
    float hitDistance = 0.0f;
    const bool hit = PickMesh(*app.loadedModel, originValue, directionValue, hitDistance);
    if (hit && !app.meshSelected)
    {
        app.meshSelected = true;
        app.status = L"Mesh selected — F frames the selection, click the background to clear";
    }
    else if (!hit && app.meshSelected)
    {
        app.meshSelected = false;
        app.status = app.readyStatus;
    }
    InvalidateRect(app.window, nullptr, FALSE);
}

void FrameSelectedOrAll(ViewerApp& app)
{
    if (!app.renderer.HasModel()) return;
    const float aspect = ViewportAspect(app);
    if (app.meshSelected && app.loadedModel)
    {
        app.camera.FrameBox(app.loadedModel->boundsMin, app.loadedModel->boundsMax, aspect);
    }
    else
    {
        app.camera.Fit(aspect);
    }
    InvalidateRect(app.window, nullptr, FALSE);
}

constexpr int kSpeedSliderMax = 100;
constexpr double kSpeedSliderMin = 0.05;   // matches Camera::kFlySpeedMin
constexpr double kSpeedSliderTop = 40.0;   // matches Camera::kFlySpeedMax

int SpeedSliderPositionFor(double speedScale)
{
    const double t = std::log(std::clamp(speedScale, kSpeedSliderMin, kSpeedSliderTop) / kSpeedSliderMin) /
        std::log(kSpeedSliderTop / kSpeedSliderMin);
    return static_cast<int>(std::lround(t * kSpeedSliderMax));
}

double SpeedForSliderPosition(int position)
{
    const double t = std::clamp(static_cast<double>(position), 0.0, static_cast<double>(kSpeedSliderMax)) / kSpeedSliderMax;
    return kSpeedSliderMin * std::pow(kSpeedSliderTop / kSpeedSliderMin, t);
}

void SyncSpeedSlider(ViewerApp& app)
{
    if (app.speedSlider) SendMessageW(app.speedSlider, TBM_SETPOS, TRUE, SpeedSliderPositionFor(app.camera.FlySpeedScale()));
}

void AdjustFlySpeed(ViewerApp& app, float wheelSteps)
{
    app.camera.SetFlySpeedScale(app.camera.FlySpeedScale() * std::pow(1.18, static_cast<double>(wheelSteps)));
    SyncSpeedSlider(app);
    ShowSpeedHud(app, L"Travel speed ×" + FormatMultiplier(app.camera.FlySpeedScale()));
}

// Guards toggle commands against keyboard auto-repeat: holding a key must not
// strobe the grid or the projection mode.
bool ConsumeToggleCommand(ViewerApp& app, int id)
{
    const double now = NowSeconds();
    if (app.lastToggleId == id && now - app.lastToggleSeconds < kToggleRepeatGuardSeconds) return false;
    app.lastToggleId = id;
    app.lastToggleSeconds = now;
    return true;
}

void ToggleGrid(ViewerApp& app)
{
    if (!CanNavigate(app) || !ConsumeToggleCommand(app, ID_VIEW_GRID)) return;
    app.gridVisible = !app.gridVisible;
    ShowModeHud(app, app.gridVisible ? L"Ground grid shown" : L"Ground grid hidden");
    if (app.gridButton) InvalidateRect(app.gridButton, nullptr, FALSE);
    InvalidateRect(app.window, nullptr, FALSE);
}

void ToggleProjection(ViewerApp& app)
{
    if (!CanNavigate(app) || !ConsumeToggleCommand(app, ID_VIEW_PROJECTION)) return;
    const bool toOrthographic = app.camera.Projection() == ProjectionMode::Perspective;
    app.camera.SetProjection(toOrthographic ? ProjectionMode::Orthographic : ProjectionMode::Perspective);
    ShowModeHud(app, toOrthographic ? L"Orthographic" : L"Perspective");
    InvalidateRect(app.window, nullptr, FALSE);
}

void ToggleAxisSnap(ViewerApp& app)
{
    if (!CanNavigate(app) || !ConsumeToggleCommand(app, ID_VIEW_AXIS_SNAP)) return;
    app.axisSnapEnabled = !app.axisSnapEnabled;
    ShowModeHud(app, app.axisSnapEnabled ? L"Axis snap on" : L"Axis snap off");
    if (app.axisSnapButton) InvalidateRect(app.axisSnapButton, nullptr, FALSE);
    InvalidateRect(app.window, nullptr, FALSE);
}

void SnapViewCommand(ViewerApp& app, ViewDir view)
{
    if (!CanNavigate(app)) return;
    // Ctrl+Numpad1/3/7 selects the reverse views (Back, Left, Bottom).
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
    {
        if (view == ViewDir::Front) view = ViewDir::Back;
        else if (view == ViewDir::Right) view = ViewDir::Left;
        else if (view == ViewDir::Top) view = ViewDir::Bottom;
    }
    app.camera.SnapToView(CanonicalViewOrientation(view));
    InvalidateRect(app.window, nullptr, FALSE);
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
    EnableWindow(app.gridButton, hasModel);
    EnableWindow(app.axisSnapButton, hasModel);
    EnableWindow(app.speedSlider, hasModel);
    const bool toolbarVisible = app.state != ViewerState::Loading;
    SetControlVisible(app.openButton, toolbarVisible);
    SetControlVisible(app.fitButton, toolbarVisible);
    SetControlVisible(app.resetButton, toolbarVisible);
    SetControlVisible(app.gridButton, toolbarVisible);
    SetControlVisible(app.axisSnapButton, toolbarVisible);
    SetControlVisible(app.speedSlider, toolbarVisible);
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
    const int resetWidth = Scale(app, 92);
    const int gridWidth = Scale(app, 56);
    const int snapWidth = Scale(app, 56);
    const int sliderWidth = Scale(app, 96);
    const int fitWidth = Scale(app, 48);
    const int openWidth = Scale(app, 68);
    int right = client.right - margin;
    MoveWindow(app.menuButton, right - menuWidth, y, menuWidth, buttonHeight, TRUE); right -= menuWidth + gap;
    MoveWindow(app.resetButton, right - resetWidth, y, resetWidth, buttonHeight, TRUE); right -= resetWidth + gap;
    MoveWindow(app.gridButton, right - gridWidth, y, gridWidth, buttonHeight, TRUE); right -= gridWidth + gap;
    MoveWindow(app.axisSnapButton, right - snapWidth, y, snapWidth, buttonHeight, TRUE); right -= snapWidth + gap;
    MoveWindow(app.speedSlider, right - sliderWidth, y, sliderWidth, buttonHeight, TRUE); right -= sliderWidth + gap;
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
    for (HWND button : { app.openButton, app.fitButton, app.resetButton, app.gridButton, app.axisSnapButton,
        app.menuButton, app.retryButton, app.openAnotherButton, app.copyButton })
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
    app.resetButton = CreateButton(app, ID_VIEW_RESET, L"Reset View");
    app.gridButton = CreateButton(app, ID_VIEW_GRID, L"Grid");
    app.axisSnapButton = CreateButton(app, ID_VIEW_AXIS_SNAP, L"Snap");
    app.speedSlider = CreateWindowExW(0, L"msctls_trackbar32", L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
        0, 0, 10, 10, app.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_VIEW_SPEED_SLIDER)), app.instance, nullptr);
    SendMessageW(app.speedSlider, TBM_SETRANGE, TRUE, MAKELONG(0, kSpeedSliderMax));
    SyncSpeedSlider(app);
    app.menuButton = CreateButton(app, ID_VIEW_MENU, L"•••");
    app.retryButton = CreateButton(app, ID_VIEW_RETRY, L"Retry");
    app.openAnotherButton = CreateButton(app, ID_VIEW_OPEN_ANOTHER, L"Open another");
    app.copyButton = CreateButton(app, ID_VIEW_COPY_DETAILS, L"Copy details");
    app.tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, app.window, nullptr, app.instance, nullptr);
    SetWindowPos(app.tooltip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SendMessageW(app.tooltip, TTM_SETMAXTIPWIDTH, 0, Scale(app, 360));
    AddTooltip(app, app.openButton, L"Open a GLB model (Ctrl+O)");
    AddTooltip(app, app.fitButton, L"Frame the model or the selection (F or Numpad .)");
    AddTooltip(app, app.resetButton, L"Reset the view to the default framing (Home)");
    AddTooltip(app, app.gridButton, L"Show or hide the ground grid (G)");
    AddTooltip(app, app.axisSnapButton, L"Snap middle-drag travel to the nearest world axis");
    AddTooltip(app, app.speedSlider, L"Travel speed (Shift while flying doubles it)");
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
    StopNavigation(app);
    EndPointer(app);
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
    EndPointer(app);
    app.failedPath.clear();
    app.errorSummary.clear();
    app.errorDetails.clear();
    app.filename = FileNameFromPath(path);
    app.status.clear();
    app.warning.clear();
    UpdateTitle(app);
    UpdateButtonAvailability(app);
    LayoutControls(app);
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
        L"Select\tClick a mesh; click the background to clear\n"
        L"Orbit\tLeft drag, gizmo ball drag, or arrow keys\n"
        L"Truck (pan)\tMiddle drag or Shift+arrow keys\n"
        L"Axis snap\tToggle the Snap button to lock truck moves to X/Z\n"
        L"Zoom\tWheel, Ctrl+middle drag, +, or -\n"
        L"Fly\tHold right mouse + W/A/S/D, Q/E; wheel or slider sets speed\n"
        L"Fly faster\tHold Shift while flying (2x)\n"
        L"Roll\tHold right mouse + Z/C\n"
        L"Front / Right / Top\tNumpad 1 / 3 / 7 (Ctrl for reverse)\n"
        L"Perspective / Ortho\tNumpad 5\n"
        L"Gizmo views\tClick an axis ball in the corner\n"
        L"Frame model / selection\tF, Numpad ., or double-click\n"
        L"Ground grid\tG\n"
        L"Reset view\tHome or R\n"
        L"Open\tCtrl+O\n"
        L"Cancel open\tEsc\n\n"
        L"Left-drag orbit and middle-drag truck wrap the cursor at the\n"
        L"viewport edge, and drags glide to a stop with exponential inertia.",
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
    const bool active = (item.CtlID == ID_VIEW_GRID && app.gridVisible) ||
        (item.CtlID == ID_VIEW_AXIS_SNAP && app.axisSnapEnabled);
    COLORREF fill = primary ? RGB(10, 132, 255) : RGB(58, 58, 60);
    COLORREF border = primary ? RGB(34, 146, 255) : RGB(73, 73, 76);
    COLORREF foreground = RGB(245, 245, 247);
    if (active) { fill = RGB(40, 44, 52); border = RGB(10, 132, 255); }
    if (hot) fill = primary ? RGB(32, 145, 255) : RGB(68, 68, 71);
    if (active && hot) fill = RGB(48, 54, 64);
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
    case ID_VIEW_FIT: FrameSelectedOrAll(app); break;
    case ID_VIEW_RESET:
        if (app.renderer.HasModel()) { app.camera.Reset(ViewportAspect(app)); InvalidateRect(app.window, nullptr, FALSE); }
        break;
    case ID_VIEW_GRID: ToggleGrid(app); break;
    case ID_VIEW_AXIS_SNAP: ToggleAxisSnap(app); break;
    case ID_VIEW_FRONT: SnapViewCommand(app, ViewDir::Front); break;
    case ID_VIEW_RIGHT: SnapViewCommand(app, ViewDir::Right); break;
    case ID_VIEW_TOP: SnapViewCommand(app, ViewDir::Top); break;
    case ID_VIEW_PROJECTION: ToggleProjection(app); break;
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

FlightInput BuildFlightInput(const ViewerApp& app)
{
    FlightInput input;
    if (!CanNavigate(app)) return input;
    // Unreal-style flight only while RMB capture is active.
    if (app.flyLook)
    {
        input.right = (app.moveRight ? 1.0f : 0.0f) - (app.moveLeft ? 1.0f : 0.0f);
        input.up = (app.moveUp ? 1.0f : 0.0f) - (app.moveDown ? 1.0f : 0.0f);
        input.forward = (app.moveForward ? 1.0f : 0.0f) - (app.moveBackward ? 1.0f : 0.0f);
        input.roll = (app.rollRight ? 1.0f : 0.0f) - (app.rollLeft ? 1.0f : 0.0f);
        input.fast = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    }
    const float arrowsX = (app.arrowRight ? 1.0f : 0.0f) - (app.arrowLeft ? 1.0f : 0.0f);
    const float arrowsY = (app.arrowDown ? 1.0f : 0.0f) - (app.arrowUp ? 1.0f : 0.0f);
    if (input.fast)
    {
        input.panX = arrowsX * kArrowPixelsPerSecond;
        input.panY = arrowsY * kArrowPixelsPerSecond;
    }
    else
    {
        input.orbitX = arrowsX * kArrowPixelsPerSecond;
        input.orbitY = arrowsY * kArrowPixelsPerSecond;
    }
    RECT client{};
    GetClientRect(app.window, &client);
    input.viewportHeight = static_cast<float>(std::max(1L, client.bottom - app.toolbarHeight));
    return input;
}

bool IsAnimating(const ViewerApp& app)
{
    const double now = NowSeconds();
    return app.state == ViewerState::Loading || HasNavigationInput(app) || app.camera.HasMotion() ||
        now < app.speedHudUntil || now < app.modeHudUntil;
}

void RenderScene(ViewerApp& app)
{
    OverlayInfo overlay;
    overlay.state = app.state;
    overlay.filename = app.filename;
    overlay.status = app.status;
    overlay.errorSummary = app.errorSummary;
    overlay.errorDetails = app.errorDetails;
    overlay.warning = app.warning;
    overlay.animationPhase = static_cast<float>(GetTickCount64() % 1400) / 1400.0f;
    overlay.dpiScale = app.dpiScale;
    overlay.toolbarHeight = app.toolbarHeight;
    overlay.hasModel = app.renderer.HasModel();
    overlay.gridVisible = app.gridVisible;
    overlay.selectionAmount = app.meshSelected ? 1.0f : 0.0f;
    const double now = NowSeconds();
    overlay.speedHud = app.speedHudText;
    overlay.speedHudAlpha = static_cast<float>(HudAlpha(app.speedHudUntil, now));
    overlay.modeHud = app.modeHudText;
    overlay.modeHudAlpha = static_cast<float>(HudAlpha(app.modeHudUntil, now));
    app.renderer.Render(app.camera, overlay, app.gizmo);
}

// Advances the camera by the wall-clock time since the last tick, from
// whichever source last ticked it (a rendered frame, or a raw mouse sample
// during fly-look — see WM_INPUT). Called once per raw mouse sample rather
// than only once per rendered frame, so WASD translation is integrated in
// step with every look update instead of catching up in one coarse jump per
// paint, which is what made turning while flying look faceted/blocky: mice
// report well above the display's refresh rate, so several look updates
// could land between two paints, all summed into a single end-of-frame
// rotation that translation then followed as one straight chord.
void TickCamera(ViewerApp& app)
{
    const double now = NowSeconds();
    double elapsed = 0.0;
    if (app.lastFrameSeconds > 0.0)
    {
        const double gap = now - app.lastFrameSeconds;
        if (gap < 0.25) elapsed = std::min(0.1, gap);
    }
    app.lastFrameSeconds = now;
    app.camera.SetInput(BuildFlightInput(app));
    app.camera.Update(elapsed);
}

void RenderFrame(ViewerApp& app)
{
    if (!app.rendererReady) return;
    TickCamera(app);
    RenderScene(app);
    ValidateRect(app.window, nullptr);
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
        {
            // Raw mouse input for fly-look: relative device deltas, not cursor-
            // position deltas, so look is not quantized to screen pixels and
            // isn't affected by Windows' pointer-acceleration curve — this is
            // what makes look feel smooth/analog instead of steppy.
            RAWINPUTDEVICE mouseDevice{};
            mouseDevice.usUsagePage = 0x01;   // HID_USAGE_PAGE_GENERIC
            mouseDevice.usUsage = 0x02;       // HID_USAGE_GENERIC_MOUSE
            mouseDevice.dwFlags = 0;
            mouseDevice.hwndTarget = window;
            RegisterRawInputDevices(&mouseDevice, 1, sizeof(mouseDevice));
        }
        CreateControls(*app);
        UpdateGizmoLayout(*app);
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
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lParam) == app->speedSlider && CanNavigate(*app))
        {
            const int position = static_cast<int>(SendMessageW(app->speedSlider, TBM_GETPOS, 0, 0));
            app->camera.SetFlySpeedScale(SpeedForSliderPosition(position));
            ShowSpeedHud(*app, L"Travel speed ×" + FormatMultiplier(app->camera.FlySpeedScale()));
        }
        return 0;
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        if (app->rendererReady)
        {
            RenderScene(*app);
        }
        else RenderFallback(*app, dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_SIZE:
        LayoutControls(*app);
        UpdateGizmoLayout(*app);
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
        UpdateGizmoLayout(*app);
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
        if (app->touchPoints.empty() && GetCapture() == window && app->pointerMode == PointerMode::None) ReleaseCapture();
        ResetTouchBaseline(*app);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            if (app->flyLook)
            {
                SetCursor(nullptr);
                return TRUE;
            }
            if (app->gizmo.hover != NavGizmo::Part::None && app->pointerMode == PointerMode::None && CanNavigate(*app))
            {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    case WM_MOUSELEAVE:
        if (app->gizmo.hover != NavGizmo::Part::None)
        {
            app->gizmo.hover = NavGizmo::Part::None;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (GET_Y_LPARAM(lParam) >= app->toolbarHeight && CanNavigate(*app))
        {
            SetFocus(window);
            const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const NavGizmo::Part part = app->gizmo.HitTest(app->camera.Orientation(),
                static_cast<float>(point.x), static_cast<float>(point.y));
            if (part == NavGizmo::Part::Ball)
            {
                SetCapture(window);
                app->pointerMode = PointerMode::GizmoOrbit;
                BeginWrappedDrag(*app, point);
                app->camera.CancelInertia();
                app->orbitVelocityX = 0.0;
                app->orbitVelocityY = 0.0;
                app->lastOrbitMoveSeconds = NowSeconds();
            }
            else if (part != NavGizmo::Part::None)
            {
                // Axis node/stem: snap immediately on press.
                app->camera.SnapToView(CanonicalViewOrientation(app->gizmo.ViewFor(part)));
                InvalidateRect(window, nullptr, FALSE);
            }
            else
            {
                // Plain LMB: orbit-drags immediately; a release that never
                // exceeded the click/drag threshold click-selects instead.
                SetCapture(window);
                app->pointerMode = PointerMode::Orbit;
                BeginWrappedDrag(*app, point);
                app->camera.CancelInertia();
                app->orbitVelocityX = 0.0;
                app->orbitVelocityY = 0.0;
                app->lastOrbitMoveSeconds = NowSeconds();
                app->selectDragged = false;
                app->selectDownPoint = point;
                app->selectDownSeconds = NowSeconds();
            }
        }
        return 0;
    case WM_MBUTTONDOWN:
        if (GET_Y_LPARAM(lParam) >= app->toolbarHeight && CanNavigate(*app))
        {
            SetFocus(window);
            SetCapture(window);
            const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            // Plain MMB trucks along the ground plane; Ctrl+MMB dollies.
            app->pointerMode = control ? PointerMode::DollyDrag : PointerMode::Truck;
            BeginWrappedDrag(*app, point);
            if (app->pointerMode == PointerMode::Truck)
            {
                app->camera.CancelInertia();
                app->panVelocityX = 0.0;
                app->panVelocityY = 0.0;
                app->lastPanMoveSeconds = NowSeconds();
            }
        }
        return 0;
    case WM_RBUTTONDOWN:
        if (GET_Y_LPARAM(lParam) >= app->toolbarHeight && CanNavigate(*app))
        {
            SetFocus(window);
            SetCapture(window);
            app->pointerMode = PointerMode::FlyLook;
            app->flyLook = true;
            GetCursorPos(&app->flyPressPoint);
            // Look is driven by WM_INPUT's raw relative deltas (registered at
            // WM_CREATE), not cursor-position deltas, so there is no screen
            // edge to fall off and nothing to recenter — just hide the cursor
            // for the duration and restore it (EndPointer) on release.
            SetCursor(nullptr);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (app->pointerMode == PointerMode::None)
        {
            // Idle hover tracking for the gizmo.
            if (CanNavigate(*app) && GET_Y_LPARAM(lParam) >= app->toolbarHeight)
            {
                const NavGizmo::Part part = app->gizmo.HitTest(app->camera.Orientation(),
                    static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)));
                if (part != app->gizmo.hover)
                {
                    app->gizmo.hover = part;
                    if (part != NavGizmo::Part::None)
                    {
                        TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
                        TrackMouseEvent(&track);
                    }
                    InvalidateRect(window, nullptr, FALSE);
                }
            }
            else if (app->gizmo.hover != NavGizmo::Part::None)
            {
                app->gizmo.hover = NavGizmo::Part::None;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        if (GetCapture() == window)
        {
            const POINT pointer{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const float deltaX = static_cast<float>(pointer.x - app->lastPointer.x);
            const float deltaY = static_cast<float>(pointer.y - app->lastPointer.y);
            app->lastPointer = pointer;
            switch (app->pointerMode)
            {
            case PointerMode::FlyLook:
                // Look is driven by WM_INPUT (raw deltas); the cursor is just
                // kept hidden here since its position is otherwise unused.
                SetCursor(nullptr);
                break;
            case PointerMode::Orbit:
            case PointerMode::GizmoOrbit:
                TrackOrbitVelocity(*app, deltaX, deltaY);
                app->camera.Orbit(deltaX, deltaY);
                WrapCursorIfNeeded(*app);
                break;
            case PointerMode::Truck:
                TrackPanVelocity(*app, deltaX, deltaY);
                app->camera.Truck(deltaX, deltaY,
                    static_cast<float>(ViewportRect(*app).bottom - ViewportRect(*app).top), app->axisSnapEnabled);
                WrapCursorIfNeeded(*app);
                break;
            case PointerMode::DollyDrag:
                app->camera.DollyDrag(deltaY);
                WrapCursorIfNeeded(*app);
                break;
            default: break;
            }
            // A plain-LMB orbit gesture also owns click-select: track whether
            // it stayed under the drag threshold, so a release without a real
            // drag still click-selects (drag and click share the button).
            if (app->pointerMode == PointerMode::Orbit &&
                std::abs(pointer.x - app->selectDownPoint.x) + std::abs(pointer.y - app->selectDownPoint.y) >
                    kClickDragThresholdPixels)
            {
                app->selectDragged = true;
            }
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (app->pointerMode == PointerMode::Orbit && !app->selectDragged && CanNavigate(*app) &&
            NowSeconds() - app->selectDownSeconds < kClickMaxSeconds)
        {
            ClickSelect(*app, app->selectDownPoint);
        }
        if ((app->pointerMode == PointerMode::Orbit || app->pointerMode == PointerMode::GizmoOrbit) &&
            CanNavigate(*app) && NowSeconds() - app->lastOrbitMoveSeconds < 0.07)
        {
            app->camera.SeedOrbitInertia(static_cast<float>(app->orbitVelocityX),
                static_cast<float>(app->orbitVelocityY));
        }
        EndPointer(*app);
        return 0;
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
        if (app->pointerMode == PointerMode::Truck && CanNavigate(*app) &&
            NowSeconds() - app->lastPanMoveSeconds < 0.07)
        {
            app->camera.SeedPanInertia(static_cast<float>(app->panVelocityX),
                static_cast<float>(app->panVelocityY));
        }
        EndPointer(*app);
        return 0;
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        EndPointer(*app);
        return 0;
    case WM_LBUTTONDBLCLK:
        if (GET_Y_LPARAM(lParam) >= app->toolbarHeight && CanNavigate(*app))
        {
            FrameSelectedOrAll(*app);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (CanNavigate(*app))
        {
            const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
            if (app->flyLook) AdjustFlySpeed(*app, steps);
            else
            {
                app->camera.Dolly(steps);
                InvalidateRect(window, nullptr, FALSE);
            }
        }
        return 0;
    case WM_INPUT:
        if (app->flyLook)
        {
            UINT size = 0;
            GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
            if (size > 0 && size <= sizeof(RAWINPUT))
            {
                RAWINPUT raw{};
                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == size &&
                    raw.header.dwType == RIM_TYPEMOUSE && (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0 &&
                    (raw.data.mouse.lLastX != 0 || raw.data.mouse.lLastY != 0))
                {
                    // Tick the camera right here, once per raw mouse sample,
                    // instead of only once per rendered frame: mice report at
                    // 125-1000Hz, well above the display refresh rate, so a
                    // render-paced tick would sum up several look updates and
                    // then move WASD translation through only their *final*
                    // orientation — a coarse, faceted approximation of the
                    // turn. Ticking per sample advances rotation and
                    // translation together at the same fine granularity, so
                    // flight curves smoothly like Unreal's.
                    app->camera.AccumulateLook(static_cast<float>(raw.data.mouse.lLastX), static_cast<float>(raw.data.mouse.lLastY));
                    TickCamera(*app);
                    InvalidateRect(window, nullptr, FALSE);
                }
            }
        }
        return DefWindowProcW(window, message, wParam, lParam);
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { CancelOpen(*app); return 0; }
        if (!CanNavigate(*app)) break;
        if (SetNavigationKey(*app, wParam, true)) return 0;
        if (wParam == VK_OEM_PLUS || wParam == VK_ADD) app->camera.Dolly(1.0f);
        else if (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT) app->camera.Dolly(-1.0f);
        else break;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_KEYUP:
        if (SetNavigationKey(*app, wParam, false)) return 0;
        break;
    case WM_KILLFOCUS:
        StopNavigation(*app);
        EndPointer(*app);
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
            app->loadedModel = complete->result.model;
            app->meshSelected = false;
            app->readyStatus = FormatCount(complete->result.model->triangleCount) +
                L" triangles  •  LMB orbit · MMB truck · RMB+WASD fly";
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
    int exitCode = 0;
    bool quitting = false;
    while (!quitting)
    {
        // Tracks whether animation was already active before each dispatched
        // message, so a message that newly turns it on gets rendered right
        // away — otherwise a key press+release landing in the same drain pass
        // (a quick tap, or the app falling briefly behind) would be fully
        // drained before the loop ever renders a frame with the key down,
        // leaving flight keys with no visible effect. Only rendering on that
        // on-transition (rather than after every message) avoids re-rendering
        // once per queued WM_MOUSEMOVE during a mouse-look drag, which would
        // otherwise serialize a burst of moves behind repeated vsync waits.
        bool wasAnimating = gMainWindow && app.rendererReady && !IsIconic(gMainWindow) && IsAnimating(app);
        // Bounded to a single burst: a self-recentering FlyLook mouse-move can
        // otherwise repost itself indefinitely (some input stacks emit a fresh
        // WM_MOUSEMOVE for every SetCursorPos, even a no-op one) and never let
        // PeekMessageW go empty, starving the render check below forever.
        int drained = 0;
        while (drained < kMaxDrainedMessagesPerIteration && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            ++drained;
            if (message.message == WM_QUIT)
            {
                exitCode = static_cast<int>(message.wParam);
                quitting = true;
                break;
            }
            // Accelerators are translated first: IsDialogMessageW would
            // otherwise consume keydowns before the hotkeys ever see them.
            if (!TranslateAcceleratorW(gMainWindow, accelerators, &message))
            {
                if (gMainWindow && IsDialogMessageW(gMainWindow, &message)) continue;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            const bool isAnimatingNow = gMainWindow && app.rendererReady && !IsIconic(gMainWindow) && IsAnimating(app);
            if (isAnimatingNow && !wasAnimating) RenderFrame(app);
            wasAnimating = isAnimatingNow;
        }
        if (quitting) break;

        // Continuous, vsync-paced rendering while anything is in motion:
        // flight keys, easing, inertia, fit/reset glides, transient HUDs,
        // or the load spinner. The loop blocks in MsgWaitForMultipleObjectsEx
        // otherwise, so a still viewport costs no CPU or GPU.
        if (gMainWindow && app.rendererReady && !IsIconic(gMainWindow) && IsAnimating(app))
        {
            RenderFrame(app);
            continue;
        }
        if (gMainWindow && GetUpdateRect(gMainWindow, nullptr, FALSE))
        {
            if (app.rendererReady && !IsIconic(gMainWindow)) RenderFrame(app);
            else RedrawWindow(gMainWindow, nullptr, nullptr, RDW_INTERNALPAINT);
            continue;
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    if (gBackgroundBrush) DeleteObject(gBackgroundBrush);
    if (SUCCEEDED(comResult)) CoUninitialize();
    return exitCode;
}
