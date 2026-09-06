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

struct Camera
{
    double targetX = 0.0;
    double targetY = 0.0;
    double targetZ = 0.0;
    double distance = 4.0;
    double sceneRadius = 1.0;
    DirectX::XMFLOAT4 orientation{ 0.0f, 0.0f, 0.0f, 1.0f };

    void SetBounds(const DirectX::XMFLOAT3& minimum, const DirectX::XMFLOAT3& maximum, float aspect);
    void Fit(float aspect);
    void Reset(float aspect);
    void Orbit(float deltaX, float deltaY);
    void Look(float deltaX, float deltaY);
    void Pan(float deltaX, float deltaY, float viewportHeight);
    void Dolly(float wheelSteps);
    void MoveLocal(float right, float up, float forward);
    void Roll(float radians);
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
