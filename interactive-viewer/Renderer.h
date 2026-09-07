#pragma once

#include "Model.h"

#include <DirectXMath.h>
#include <windows.h>

#include <memory>
#include <string>

enum class ViewerState
{
    Empty,
    Loading,
    Ready,
    Failed
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

struct Camera
{
    double targetX = 0.0;
    double targetY = 0.0;
    double targetZ = 0.0;
    double distance = 4.0;
    double targetDistance = 4.0;
    double sceneRadius = 1.0;
    DirectX::XMFLOAT4 orientation{ 0.0f, 0.0f, 0.0f, 1.0f };

    double homeX = 0.0;
    double homeY = 0.0;
    double homeZ = 0.0;
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
    FlightInput input{};

    void SetBounds(const DirectX::XMFLOAT3& minimum, const DirectX::XMFLOAT3& maximum, float aspect);
    void Fit(float aspect);
    void Reset(float aspect);
    void Orbit(float deltaX, float deltaY);
    void Look(float deltaX, float deltaY);
    void Pan(float deltaX, float deltaY, float viewportHeight);
    void Dolly(float wheelSteps);
    void MoveLocal(float rightAmount, float upAmount, float forwardAmount);
    void Roll(float radians);
    void SetInput(const FlightInput& value);
    void Update(double deltaTime);
    bool HasMotion() const;
    void StopMotion();
    void SeedOrbitInertia(float velocityX, float velocityY);
    void CancelInertia();

private:
    double FitDistance(float aspect) const;
    double FlightSpeed() const;
    void ApplyOrbitAngles(float yawAngle, float pitchAngle);
    void ShiftPivot(double x, double y, double z);
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
    void Render(const Camera& camera, const OverlayInfo& overlay);
    bool HasModel() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
