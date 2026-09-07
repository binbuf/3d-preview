#pragma once

#include "Model.h"
#include "NavGizmo.h"

#include <DirectXMath.h>
#include <windows.h>

#include <memory>
#include <string>

// Vertical field of view shared by the camera math, framing, and picking.
constexpr float kVerticalFieldOfView = DirectX::XM_PIDIV4;

enum class ViewerState
{
    Empty,
    Loading,
    Ready,
    Failed
};

enum class ProjectionMode
{
    Perspective,
    Orthographic
};

struct OverlayInfo
{
    ViewerState state = ViewerState::Empty;
    std::wstring filename;
    std::wstring status;
    std::wstring errorSummary;
    std::wstring errorDetails;
    std::wstring warning;
    float animationPhase = 0.0f;
    float dpiScale = 1.0f;
    int toolbarHeight = 40;
    bool hasModel = false;
    bool gridVisible = true;
    float selectionAmount = 0.0f;   // 0..1 mesh-selection highlight
    std::wstring speedHud;          // transient fly-speed readout
    float speedHudAlpha = 0.0f;
    std::wstring modeHud;           // transient mode readout (grid/projection)
    float modeHudAlpha = 0.0f;
};

// Per-frame navigation intents gathered from keyboard state. The camera eases
// toward these each frame, which is what makes flight feel weighty instead of
// toggling velocity on and off.
struct FlightInput
{
    float right = 0.0f;      // -1..1 strafe intent
    float up = 0.0f;        // -1..1 vertical intent
    float forward = 0.0f;  // -1..1 view-relative intent
    float roll = 0.0f;      // -1..1 roll intent
    float orbitX = 0.0f;    // pixels/second orbit intent (arrow keys)
    float orbitY = 0.0f;
    float panX = 0.0f;      // pixels/second pan intent (Shift+arrow keys)
    float panY = 0.0f;
    bool fast = false;      // Shift boost
    float viewportHeight = 0.0f;
};

// Turntable orbit camera. The orientation is a quaternion (world-from-camera
// rotation), which sidesteps Euler gimbal lock entirely: yaw composes about the
// world up axis and pitch about the camera right axis, both as axis-angle
// quaternion multiplies. The eye is always pivot + rotate((0, 0, distance), q).
struct Camera
{
    double targetX = 0.0;
    double targetY = 0.0;
    double targetZ = 0.0;
    double distance = 4.0;
    double targetDistance = 4.0;
    double sceneRadius = 1.0;
    DirectX::XMFLOAT4 orientation{ 0.0f, 0.0f, 0.0f, 1.0f };

    // Cached default framing captured when a model loads. "Reset View"
    // (Home / the toolbar button) interpolates the camera back to this state.
    double homeX = 0.0;
    double homeY = 0.0;
    double homeZ = 0.0;
    double homeDistance = 4.0;
    DirectX::XMFLOAT3 homeBoundsMin{};
    DirectX::XMFLOAT3 homeBoundsMax{};
    DirectX::XMFLOAT4 homeOrientation{ 0.0f, 0.0f, 0.0f, 1.0f };

    double desiredX = 0.0;
    double desiredY = 0.0;
    double desiredZ = 0.0;
    bool pivotAnimating = false;
    double pivotAnimElapsed = 0.0;
    double pivotFromX = 0.0;
    double pivotFromY = 0.0;
    double pivotFromZ = 0.0;
    DirectX::XMFLOAT4 desiredOrientation{ 0.0f, 0.0f, 0.0f, 1.0f };
    bool orientationAnimating = false;
    double orientationAnimElapsed = 0.0;
    DirectX::XMFLOAT4 orientationFrom{ 0.0f, 0.0f, 0.0f, 1.0f };

    // Perspective/orthographic toggle. The orthographic half-height equals
    // distance * tan(fov/2), so switching projection preserves the framing at
    // the pivot plane and wheel dolly means the same thing in both modes.
    ProjectionMode projection = ProjectionMode::Perspective;
    double projectionBlend = 0.0;  // eased 0..1 cross-fade of the two matrices

    double velRight = 0.0;
    double velUp = 0.0;
    double velForward = 0.0;
    double rollRate = 0.0;
    double orbitRateX = 0.0;
    double orbitRateY = 0.0;
    double panRateX = 0.0;
    double panRateY = 0.0;
    double inertiaX = 0.0;
    double inertiaY = 0.0;
    double panInertiaX = 0.0;
    double panInertiaY = 0.0;
    double lastViewportHeight = 0.0;
    double flySpeedScale = 1.0;
    FlightInput input{};

    void SetBounds(const DirectX::XMFLOAT3& minimum, const DirectX::XMFLOAT3& maximum, float aspect);
    void Fit(float aspect);
    // Frames an arbitrary world-space box (used for "frame selected").
    void FrameBox(const DirectX::XMFLOAT3& minimum, const DirectX::XMFLOAT3& maximum, float aspect);
    void Reset(float aspect);
    void Orbit(float deltaX, float deltaY);
    void Look(float deltaX, float deltaY);
    void Pan(float deltaX, float deltaY, float viewportHeight);
    // Trucks along the world ground plane (flattened forward/right), optionally
    // snapped to the nearest world axis. Used by the MMB drag and Shift+arrows.
    void Truck(float deltaX, float deltaY, float viewportHeight, bool axisSnap);
    void Dolly(float wheelSteps);
    // Smooth continuous dolly driven by a Ctrl+MMB drag.
    void DollyDrag(float deltaY);
    void MoveLocal(float rightAmount, float upAmount, float forwardAmount);
    void Roll(float radians);
    void SetInput(const FlightInput& value);
    void Update(double deltaTime);
    bool HasMotion() const;
    void StopMotion();
    void SeedOrbitInertia(float velocityX, float velocityY);
    void SeedPanInertia(float velocityX, float velocityY);
    void CancelInertia();
    // Animates orientation to an axis-aligned view (shortest-arc slerp).
    void SnapToView(DirectX::XMVECTOR viewOrientation);
    void SetProjection(ProjectionMode mode);
    ProjectionMode Projection() const { return projection; }
    void SetFlySpeedScale(double scale);
    double FlySpeedScale() const { return flySpeedScale; }

    DirectX::XMVECTOR Orientation() const;
    DirectX::XMVECTOR EyePosition() const;
    DirectX::XMMATRIX ViewMatrix() const;
    // Blends perspective and orthographic according to projectionBlend.
    DirectX::XMMATRIX ProjectionMatrix(float aspect) const;

private:
    double FitDistance(float aspect) const;
    double FlightSpeed() const;
    void ApplyOrbitAngles(float yawAngle, float pitchAngle);
    void ShiftPivot(double x, double y, double z);
    double FarBound() const;
};

RECT CalculateErrorCardRect(int width, int height, int toolbarHeight, float dpiScale);

class Renderer
{
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool Initialize(HWND window, std::wstring& error);
    bool Resize(int width, int height, std::wstring& error);
    bool UploadModel(const ModelData& model, std::wstring& error);
    void ClearModel();
    void Render(const Camera& camera, const OverlayInfo& overlay, const NavGizmo& gizmo);
    bool HasModel() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
