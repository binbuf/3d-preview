#include "framework.h"
#include "Renderer.h"

#include <d2d1.h>
#include <d2d1helper.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dwrite.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <array>
#include <sstream>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
float Scale(float value, float dpiScale)
{
    return std::round(value * dpiScale);
}

// Vector glyphs for the toolbar/status-bar icon buttons — hand-drawn with
// plain D2D primitives (lines/ellipses/rectangles) rather than a bitmap
// asset, so they stay pixel-crisp at any DPI and need no image-loading
// pipeline, matching how the system caption glyphs (DrawTitleBar) and the
// NavGizmo axis balls are already drawn.
enum class IconKind
{
    Grid,
    AxisSnap,
    Speed,
    Fit,
    Reset,
    Share,
    Overflow,
    OpenWith,
    Info,
    FullscreenEnter,
    FullscreenExit,
};

// Four L-shaped corner brackets around (cx, cy). `vertexRadius` is where each
// bracket's corner sits and `armEndRadius` is how far its two arms reach;
// vertexRadius > armEndRadius points the brackets inward (Fit / enter
// fullscreen, brackets at the outer edge closing toward the middle) while
// vertexRadius < armEndRadius points them outward (exit fullscreen, brackets
// near the middle opening toward the edge).
void DrawCornerBrackets(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush, float cx, float cy,
    float vertexRadius, float armEndRadius, float strokeWidth)
{
    for (float sx : { -1.0f, 1.0f })
    {
        for (float sy : { -1.0f, 1.0f })
        {
            const D2D1_POINT_2F vertex{ cx + sx * vertexRadius, cy + sy * vertexRadius };
            target->DrawLine(vertex, D2D1::Point2F(cx + sx * armEndRadius, cy + sy * vertexRadius), brush, strokeWidth);
            target->DrawLine(vertex, D2D1::Point2F(cx + sx * vertexRadius, cy + sy * armEndRadius), brush, strokeWidth);
        }
    }
}

void DrawIcon(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush, IconKind kind, D2D1_RECT_F rect, float scale)
{
    const float cx = (rect.left + rect.right) * 0.5f;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    const float stroke = Scale(1.4f, scale);
    switch (kind)
    {
    case IconKind::Grid:
    {
        const float half = Scale(7.0f, scale);
        target->DrawRectangle(D2D1::RectF(cx - half, cy - half, cx + half, cy + half), brush, stroke);
        target->DrawLine(D2D1::Point2F(cx, cy - half), D2D1::Point2F(cx, cy + half), brush, stroke);
        target->DrawLine(D2D1::Point2F(cx - half, cy), D2D1::Point2F(cx + half, cy), brush, stroke);
        break;
    }
    case IconKind::AxisSnap:
    {
        // Target/crosshair with four tick marks poking outside the ring.
        const float radius = Scale(5.5f, scale);
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush, stroke);
        const float tickOuter = radius + Scale(3.0f, scale);
        const float tickInner = radius - Scale(1.5f, scale);
        target->DrawLine(D2D1::Point2F(cx, cy - tickOuter), D2D1::Point2F(cx, cy - tickInner), brush, stroke);
        target->DrawLine(D2D1::Point2F(cx, cy + tickInner), D2D1::Point2F(cx, cy + tickOuter), brush, stroke);
        target->DrawLine(D2D1::Point2F(cx - tickOuter, cy), D2D1::Point2F(cx - tickInner, cy), brush, stroke);
        target->DrawLine(D2D1::Point2F(cx + tickInner, cy), D2D1::Point2F(cx + tickOuter, cy), brush, stroke);
        break;
    }
    case IconKind::Speed:
    {
        // Speedometer: an upper-half-circle gauge face with a needle.
        const float radius = Scale(6.5f, scale);
        const float baseY = cy + Scale(1.5f, scale);
        constexpr int segments = 10;
        D2D1_POINT_2F previous{};
        for (int i = 0; i <= segments; ++i)
        {
            const float t = static_cast<float>(i) / segments;
            const float angle = XM_PI + t * XM_PI;
            const D2D1_POINT_2F point{ cx + std::cos(angle) * radius, baseY + std::sin(angle) * radius };
            if (i > 0) target->DrawLine(previous, point, brush, stroke);
            previous = point;
        }
        constexpr float needleAngle = -XM_PIDIV2 + 0.75f;
        target->DrawLine(D2D1::Point2F(cx, baseY),
            D2D1::Point2F(cx + std::cos(needleAngle) * radius * 0.8f, baseY + std::sin(needleAngle) * radius * 0.8f),
            brush, stroke);
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, baseY), Scale(1.3f, scale), Scale(1.3f, scale)), brush);
        break;
    }
    case IconKind::Fit:
    {
        // A picture-frame with a small mountain silhouette inside, reading as
        // "frame the subject" — deliberately distinct from the corner-bracket
        // expand glyph used for Fullscreen (below) so the two read as separate
        // actions instead of duplicates.
        const float halfW = Scale(7.5f, scale);
        const float halfH = Scale(6.0f, scale);
        target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - halfW, cy - halfH, cx + halfW, cy + halfH),
            Scale(2.0f, scale), Scale(2.0f, scale)), brush, stroke);
        const float baseY = cy + halfH - Scale(1.5f, scale);
        const D2D1_POINT_2F peak{ cx - Scale(1.0f, scale), cy - Scale(1.0f, scale) };
        const D2D1_POINT_2F left{ cx - halfW + Scale(2.0f, scale), baseY };
        const D2D1_POINT_2F right{ cx + halfW - Scale(2.0f, scale), baseY };
        target->DrawLine(left, peak, brush, stroke);
        target->DrawLine(peak, right, brush, stroke);
        target->DrawLine(left, right, brush, stroke);
        break;
    }
    case IconKind::FullscreenEnter:
        DrawCornerBrackets(target, brush, cx, cy, Scale(6.5f, scale), Scale(3.2f, scale), stroke);
        break;
    case IconKind::FullscreenExit:
        DrawCornerBrackets(target, brush, cx, cy, Scale(2.6f, scale), Scale(6.5f, scale), stroke);
        break;
    case IconKind::Reset:
    {
        // Circular refresh arrow: a ~280-degree arc with an arrowhead at the
        // trailing end, tangent to the direction of travel.
        const float radius = Scale(6.5f, scale);
        constexpr int segments = 12;
        constexpr float startAngle = -XM_PIDIV2 - 0.35f;
        constexpr float sweep = XM_2PI * 0.78f;
        D2D1_POINT_2F previous{};
        D2D1_POINT_2F tip{};
        float tipAngle = 0.0f;
        for (int i = 0; i <= segments; ++i)
        {
            const float t = static_cast<float>(i) / segments;
            const float angle = startAngle + t * sweep;
            const D2D1_POINT_2F point{ cx + std::cos(angle) * radius, cy + std::sin(angle) * radius };
            if (i > 0) target->DrawLine(previous, point, brush, stroke);
            previous = point;
            tip = point;
            tipAngle = angle;
        }
        const float tangent = tipAngle + XM_PIDIV2;
        const float headSize = Scale(3.2f, scale);
        for (float sign : { -1.0f, 1.0f })
        {
            const float headAngle = tangent + XM_PI + sign * 0.55f;
            target->DrawLine(tip, D2D1::Point2F(tip.x + std::cos(headAngle) * headSize, tip.y + std::sin(headAngle) * headSize),
                brush, stroke);
        }
        break;
    }
    case IconKind::Info:
    {
        const float radius = Scale(7.0f, scale);
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush, stroke);
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy - radius * 0.42f), Scale(1.15f, scale), Scale(1.15f, scale)), brush);
        target->DrawLine(D2D1::Point2F(cx, cy - radius * 0.02f), D2D1::Point2F(cx, cy + radius * 0.5f), brush, Scale(1.7f, scale));
        break;
    }
    case IconKind::Share:
    {
        const float halfW = Scale(6.0f, scale);
        const float boxTop = cy + Scale(1.0f, scale);
        const float boxBottom = cy + Scale(6.5f, scale);
        target->DrawLine(D2D1::Point2F(cx - halfW, boxTop), D2D1::Point2F(cx - halfW, boxBottom), brush, stroke);
        target->DrawLine(D2D1::Point2F(cx + halfW, boxTop), D2D1::Point2F(cx + halfW, boxBottom), brush, stroke);
        target->DrawLine(D2D1::Point2F(cx - halfW, boxBottom), D2D1::Point2F(cx + halfW, boxBottom), brush, stroke);
        const float arrowTop = cy - Scale(7.0f, scale);
        target->DrawLine(D2D1::Point2F(cx, boxTop + Scale(0.5f, scale)), D2D1::Point2F(cx, arrowTop), brush, stroke);
        const float headSize = Scale(3.0f, scale);
        target->DrawLine(D2D1::Point2F(cx, arrowTop), D2D1::Point2F(cx - headSize, arrowTop + headSize), brush, stroke);
        target->DrawLine(D2D1::Point2F(cx, arrowTop), D2D1::Point2F(cx + headSize, arrowTop + headSize), brush, stroke);
        break;
    }
    case IconKind::Overflow:
    {
        const float dotRadius = Scale(1.6f, scale);
        const float gapX = Scale(5.0f, scale);
        for (float i : { -1.0f, 0.0f, 1.0f })
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + i * gapX, cy), dotRadius, dotRadius), brush);
        break;
    }
    case IconKind::OpenWith:
    {
        const float half = Scale(4.6f, scale);
        const float boxCx = cx - Scale(6.0f, scale);
        const float boxCy = cy + Scale(1.0f, scale);
        target->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(boxCx - half, boxCy - half, boxCx + half, boxCy + half), Scale(1.5f, scale), Scale(1.5f, scale)),
            brush, stroke);
        const D2D1_POINT_2F arrowTip{ boxCx + half + Scale(2.2f, scale), boxCy - half - Scale(1.2f, scale) };
        target->DrawLine(D2D1::Point2F(boxCx + Scale(1.0f, scale), boxCy - Scale(1.0f, scale)), arrowTip, brush, stroke);
        const float headSize = Scale(2.6f, scale);
        target->DrawLine(arrowTip, D2D1::Point2F(arrowTip.x - headSize, arrowTip.y), brush, stroke);
        target->DrawLine(arrowTip, D2D1::Point2F(arrowTip.x, arrowTip.y + headSize), brush, stroke);
        const float chevCx = cx + Scale(7.0f, scale);
        const float chevHalf = Scale(2.4f, scale);
        target->DrawLine(D2D1::Point2F(chevCx - chevHalf, cy - Scale(1.5f, scale)), D2D1::Point2F(chevCx, cy + Scale(1.5f, scale)), brush, stroke);
        target->DrawLine(D2D1::Point2F(chevCx, cy + Scale(1.5f, scale)), D2D1::Point2F(chevCx + chevHalf, cy - Scale(1.5f, scale)), brush, stroke);
        break;
    }
    }
}

XMVECTOR LoadOrientation(const Camera& camera)
{
    return XMQuaternionNormalize(XMLoadFloat4(&camera.orientation));
}

ComPtr<ID3DBlob> CompileShader(const char* source, const char* entry, const char* target, std::wstring& error)
{
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
    ComPtr<ID3DBlob> shader;
    ComPtr<ID3DBlob> messages;
    const HRESULT result = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, entry, target,
        flags, 0, &shader, &messages);
    if (FAILED(result))
    {
        error = L"The built-in graphics shader could not be compiled.";
        if (messages)
        {
            const char* text = static_cast<const char*>(messages->GetBufferPointer());
            const int needed = MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(messages->GetBufferSize()), nullptr, 0);
            if (needed > 0)
            {
                std::wstring details(needed, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(messages->GetBufferSize()), details.data(), needed);
                error += L" " + details;
            }
        }
        return nullptr;
    }
    return shader;
}

const char* kVertexShader = R"(
cbuffer Frame : register(b0)
{
    row_major float4x4 viewProjection;
    float4 cameraPosition;
    float4 options;
};

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 color : COLOR;
};

struct VertexOutput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 color : COLOR;
};

VertexOutput main(VertexInput input)
{
    VertexOutput output;
    output.position = mul(float4(input.position, 1.0), viewProjection);
    output.worldPosition = input.position;
    output.normal = input.normal;
    output.color = input.color;
    return output;
}
)";

const char* kPixelShader = R"(
cbuffer Frame : register(b0)
{
    row_major float4x4 viewProjection;
    float4 cameraPosition;
    float4 options;
};

struct PixelInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 color : COLOR;
};

float4 main(PixelInput input) : SV_TARGET
{
    float3 normal = normalize(input.normal);
    float3 viewDirection = normalize(cameraPosition.xyz - input.worldPosition);
    float3 key = normalize(float3(-0.45, 0.82, -0.35));
    float3 fill = normalize(float3(0.65, 0.25, 0.70));
    float keyLight = saturate(dot(normal, key));
    float fillLight = saturate(dot(normal, fill));
    float hemisphere = lerp(0.18, 0.42, normal.y * 0.5 + 0.5);
    float rim = pow(1.0 - saturate(dot(normal, viewDirection)), 3.0) * 0.12;
    float3 lit = input.color.rgb * (hemisphere + keyLight * 0.72 + fillLight * 0.18) + rim;
    // Mesh-selection highlight: a cool fresnel lift driven by options.x.
    float outline = pow(1.0 - saturate(dot(normal, viewDirection)), 2.0);
    lit += options.x * (outline * 0.45 * float3(0.36, 0.62, 1.0) + 0.03);
    return float4(lit, input.color.a);
}
)";

struct FrameConstants
{
    XMFLOAT4X4 viewProjection{};
    XMFLOAT4 cameraPosition{};
    XMFLOAT4 options{};
};
}

namespace
{
constexpr float kOrbitPixelsToRadians = 0.008f;
constexpr float kLookPixelsToRadians = 0.006f;
constexpr double kFlightAccelSeconds = 0.13;
constexpr double kRollAccelSeconds = 0.10;
constexpr double kRollBaseSpeed = 1.2;
constexpr double kBoostMultiplier = 2.0;
constexpr double kInertiaDecaySeconds = 0.5;
constexpr double kInertiaMinPixelsPerSecond = 12.0;
constexpr double kInertiaMaxPixelsPerSecond = 420.0;
constexpr double kZoomEaseSeconds = 0.09;
constexpr double kPivotAnimSeconds = 0.5;
constexpr double kOrientationAnimSeconds = 0.55;
// Perspective/orthographic cross-fade duration.
constexpr double kProjectionBlendSeconds = 0.16;
// Ctrl+MMB drag: log-distance change per dragged pixel.
constexpr double kDollyDragPerPixel = 0.0045;
// Orbit and look pitch clamp: |forward . worldUp| may not exceed this, which
// keeps the turntable and freelook from crossing the up-axis pole and
// flipping the horizon. Yaw is never restricted.
constexpr float kPitchPoleLimit = 0.99998f;
constexpr double kFlySpeedMin = 0.05;
constexpr double kFlySpeedMax = 40.0;

double EaseFactor(double deltaSeconds, double tau)
{
    return deltaSeconds > 0.0 ? 1.0 - std::exp(-deltaSeconds / tau) : 0.0;
}

XMFLOAT4 DefaultOrientation()
{
    XMFLOAT4 result{};
    XMStoreFloat4(&result, XMQuaternionRotationRollPitchYaw(-0.30f, 0.58f, 0.0f));
    return result;
}
}

void Camera::SetBounds(const XMFLOAT3& minimum, const XMFLOAT3& maximum, float aspect)
{
    homeX = (static_cast<double>(minimum.x) + maximum.x) * 0.5;
    homeY = (static_cast<double>(minimum.y) + maximum.y) * 0.5;
    homeZ = (static_cast<double>(minimum.z) + maximum.z) * 0.5;
    const double extentX = static_cast<double>(maximum.x) - minimum.x;
    const double extentY = static_cast<double>(maximum.y) - minimum.y;
    const double extentZ = static_cast<double>(maximum.z) - minimum.z;
    sceneRadius = std::max(0.0001, std::sqrt(extentX * extentX + extentY * extentY + extentZ * extentZ) * 0.5);
    targetX = homeX;
    targetY = homeY;
    targetZ = homeZ;
    homeOrientation = DefaultOrientation();
    orientation = homeOrientation;
    desiredOrientation = homeOrientation;
    homeBoundsMin = minimum;
    homeBoundsMax = maximum;
    distance = FitDistance(aspect);
    targetDistance = distance;
    homeDistance = distance;  // cached default framing for Reset View
    pivotAnimating = false;
    orientationAnimating = false;
    StopMotion();
}

double Camera::FitDistance(float aspect) const
{
    const double vertical = kVerticalFieldOfView;
    const double horizontal = 2.0 * std::atan(std::tan(vertical * 0.5) * std::max(0.1f, aspect));
    const double limitingFov = std::min(vertical, horizontal);
    const double value = sceneRadius / std::max(0.05, std::sin(limitingFov * 0.5)) / 0.93;
    return std::clamp(value, sceneRadius * 0.05, sceneRadius * 250.0);
}

void Camera::Fit(float aspect)
{
    FrameBox(homeBoundsMin, homeBoundsMax, aspect);
}

void Camera::FrameBox(const XMFLOAT3& minimum, const XMFLOAT3& maximum, float aspect)
{
    desiredX = (static_cast<double>(minimum.x) + maximum.x) * 0.5;
    desiredY = (static_cast<double>(minimum.y) + maximum.y) * 0.5;
    desiredZ = (static_cast<double>(minimum.z) + maximum.z) * 0.5;
    const double extentX = static_cast<double>(maximum.x) - minimum.x;
    const double extentY = static_cast<double>(maximum.y) - minimum.y;
    const double extentZ = static_cast<double>(maximum.z) - minimum.z;
    const double radius = std::max(0.0001, std::sqrt(extentX * extentX + extentY * extentY + extentZ * extentZ) * 0.5);
    const double vertical = kVerticalFieldOfView;
    const double horizontal = 2.0 * std::atan(std::tan(vertical * 0.5) * std::max(0.1f, aspect));
    const double limiting = std::min(vertical, horizontal);
    targetDistance = std::clamp(radius / std::max(0.05, std::sin(limiting * 0.5)) / 0.93,
        radius * 0.05, radius * 250.0);
    pivotFromX = targetX;
    pivotFromY = targetY;
    pivotFromZ = targetZ;
    pivotAnimElapsed = 0.0;
    pivotAnimating = true;
    CancelInertia();
}

void Camera::Reset(float aspect)
{
    Fit(aspect);
    desiredOrientation = homeOrientation;
    orientationFrom = orientation;
    orientationAnimElapsed = 0.0;
    orientationAnimating = true;
    // Reset also restores the cached default projection mode.
    SetProjection(ProjectionMode::Perspective);
}

void Camera::SnapToView(XMVECTOR viewOrientation)
{
    XMStoreFloat4(&desiredOrientation, XMQuaternionNormalize(viewOrientation));
    orientationFrom = orientation;
    orientationAnimElapsed = 0.0;
    orientationAnimating = true;
    CancelInertia();
}

void Camera::SetProjection(ProjectionMode mode)
{
    projection = mode;
}

void Camera::SetFlySpeedScale(double scale)
{
    flySpeedScale = std::clamp(scale, kFlySpeedMin, kFlySpeedMax);
}

void Camera::ApplyOrbitAngles(float yawAngle, float pitchAngle)
{
    XMVECTOR current = LoadOrientation(*this);
    const XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
    // Turntable orbit: yaw about the world up axis, pitch about the camera
    // right axis. Composed as quaternion multiplies there is no Euler order
    // and no gimbal lock; the pole clamp below only keeps the horizon from
    // flipping when the view would cross straight over the top.
    current = XMQuaternionNormalize(XMQuaternionMultiply(current,
        XMQuaternionRotationAxis(worldUp, yawAngle)));
    if (pitchAngle != 0.0f)
    {
        const XMVECTOR right = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), current);
        const XMVECTOR pitched = XMQuaternionNormalize(XMQuaternionMultiply(current,
            XMQuaternionRotationAxis(right, pitchAngle)));
        const XMVECTOR forward = XMVector3Rotate(XMVectorSet(0, 0, -1, 0), pitched);
        if (std::abs(XMVectorGetX(XMVector3Dot(forward, worldUp))) <= kPitchPoleLimit)
        {
            current = pitched;
        }
    }
    XMStoreFloat4(&orientation, current);
    orientationAnimating = false;
}

void Camera::Orbit(float deltaX, float deltaY)
{
    ApplyOrbitAngles(-deltaX * kOrbitPixelsToRadians, -deltaY * kOrbitPixelsToRadians);
}

void Camera::Look(float deltaX, float deltaY)
{
    XMVECTOR current = LoadOrientation(*this);
    const XMVECTOR target = XMVectorSet(static_cast<float>(targetX), static_cast<float>(targetY),
        static_cast<float>(targetZ), 1.0f);
    const XMVECTOR eye = target + XMVector3Rotate(XMVectorSet(0, 0, static_cast<float>(distance), 0), current);

    const XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
    // Yaw about world up, not the camera's local up: once the camera is
    // pitched, its local up axis is tilted, and yawing about it banks the
    // horizon instead of turning level. Unreal's editor free-look (and our
    // own orbit drag, see ApplyOrbitAngles) always yaws about world up so
    // the camera stays level to the ground plane through any pitch.
    const XMVECTOR yaw = XMQuaternionRotationAxis(worldUp, -deltaX * kLookPixelsToRadians);
    current = XMQuaternionNormalize(XMQuaternionMultiply(current, yaw));
    const XMVECTOR right = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), current);
    const XMVECTOR pitched = XMQuaternionNormalize(XMQuaternionMultiply(current,
        XMQuaternionRotationAxis(right, -deltaY * kLookPixelsToRadians)));
    const XMVECTOR forward = XMVector3Rotate(XMVectorSet(0, 0, -1, 0), pitched);
    // Unreal-style freelook: yaw freely, but clamp pitch at the horizon pole.
    current = std::abs(XMVectorGetX(XMVector3Dot(forward, worldUp))) > kPitchPoleLimit
        ? current : pitched;

    const XMVECTOR newTarget = eye - XMVector3Rotate(
        XMVectorSet(0, 0, static_cast<float>(distance), 0), current);
    XMFLOAT3 targetValue{};
    XMStoreFloat3(&targetValue, newTarget);
    targetX = targetValue.x;
    targetY = targetValue.y;
    targetZ = targetValue.z;
    pivotAnimating = false;
    orientationAnimating = false;
    XMStoreFloat4(&orientation, current);
}

void Camera::AccumulateLook(float deltaX, float deltaY)
{
    pendingLookX += deltaX;
    pendingLookY += deltaY;
}

void Camera::ShiftPivot(double x, double y, double z)
{
    targetX += x;
    targetY += y;
    targetZ += z;
    if (pivotAnimating)
    {
        pivotFromX += x;
        pivotFromY += y;
        pivotFromZ += z;
        desiredX += x;
        desiredY += y;
        desiredZ += z;
    }
}

void Camera::Pan(float deltaX, float deltaY, float viewportHeight)
{
    if (viewportHeight <= 1.0f) return;
    if (viewportHeight > 1.0f) lastViewportHeight = viewportHeight;
    const XMVECTOR current = LoadOrientation(*this);
    XMFLOAT3 right{};
    XMFLOAT3 up{};
    XMStoreFloat3(&right, XMVector3Rotate(XMVectorSet(1, 0, 0, 0), current));
    XMStoreFloat3(&up, XMVector3Rotate(XMVectorSet(0, 1, 0, 0), current));
    const double unitsPerPixel = 2.0 * distance * std::tan(kVerticalFieldOfView * 0.5) / viewportHeight;
    ShiftPivot((-right.x * deltaX + up.x * deltaY) * unitsPerPixel,
        (-right.y * deltaX + up.y * deltaY) * unitsPerPixel,
        (-right.z * deltaX + up.z * deltaY) * unitsPerPixel);
}

void Camera::Truck(float deltaX, float deltaY, float viewportHeight, bool axisSnap)
{
    if (viewportHeight <= 1.0f) return;
    lastViewportHeight = viewportHeight;
    const XMVECTOR current = LoadOrientation(*this);
    const XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
    const XMVECTOR forward3 = XMVector3Rotate(XMVectorSet(0, 0, -1, 0), current);
    const XMVECTOR right3 = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), current);
    XMFLOAT3 forwardValue{};
    XMFLOAT3 rightValue{};
    XMStoreFloat3(&forwardValue, forward3);
    XMStoreFloat3(&rightValue, right3);

    XMVECTOR flatForward = XMVectorSet(forwardValue.x, 0, forwardValue.z, 0);
    if (XMVectorGetX(XMVector3LengthSq(flatForward)) < 1e-6f)
    {
        // Looking straight up/down: right stays horizontal (pitch is applied
        // about it), so derive forward from it instead of leaving it
        // undefined, keeping truck direction continuous through the pole.
        flatForward = XMVector3Cross(worldUp, XMVectorSet(rightValue.x, 0, rightValue.z, 0));
    }
    flatForward = XMVector3Normalize(flatForward);
    XMVECTOR flatRight = XMVector3Normalize(XMVectorSet(rightValue.x, 0, rightValue.z, 0));
    if (axisSnap)
    {
        const float yaw = std::atan2(XMVectorGetX(flatForward), XMVectorGetZ(flatForward));
        const float snapped = std::round(yaw / XM_PIDIV2) * XM_PIDIV2;
        flatForward = XMVectorSet(std::sin(snapped), 0, std::cos(snapped), 0);
        flatRight = XMVector3Cross(flatForward, worldUp);
    }

    XMFLOAT3 forwardFlat{};
    XMFLOAT3 rightFlat{};
    XMStoreFloat3(&forwardFlat, flatForward);
    XMStoreFloat3(&rightFlat, flatRight);
    const double unitsPerPixel = 2.0 * distance * std::tan(kVerticalFieldOfView * 0.5) / viewportHeight;
    ShiftPivot((-rightFlat.x * deltaX + forwardFlat.x * deltaY) * unitsPerPixel, 0.0,
        (-rightFlat.z * deltaX + forwardFlat.z * deltaY) * unitsPerPixel);
}

void Camera::Dolly(float wheelSteps)
{
    targetDistance *= std::exp(-static_cast<double>(wheelSteps) * 0.16);
    targetDistance = std::clamp(targetDistance, sceneRadius * 0.025, sceneRadius * 250.0);
}

void Camera::DollyDrag(float deltaY)
{
    // Ctrl+MMB drag: pull up to close in, push down to pull back. Exponential
    // in the drag distance, so the response feels proportional at any scale.
    targetDistance = std::clamp(targetDistance * std::exp(static_cast<double>(deltaY) * kDollyDragPerPixel),
        sceneRadius * 0.025, sceneRadius * 250.0);
}

void Camera::MoveLocal(float rightAmount, float upAmount, float forwardAmount)
{
    const XMVECTOR current = LoadOrientation(*this);
    const XMVECTOR right = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), current);
    const XMVECTOR up = XMVector3Rotate(XMVectorSet(0, 1, 0, 0), current);
    const XMVECTOR forward = XMVector3Rotate(XMVectorSet(0, 0, -1, 0), current);
    XMFLOAT3 movement{};
    XMStoreFloat3(&movement, right * rightAmount + up * upAmount + forward * forwardAmount);
    ShiftPivot(movement.x, movement.y, movement.z);
}

void Camera::Roll(float radians)
{
    XMVECTOR current = LoadOrientation(*this);
    const XMVECTOR forward = XMVector3Rotate(XMVectorSet(0, 0, -1, 0), current);
    const XMVECTOR roll = XMQuaternionRotationAxis(forward, radians);
    current = XMQuaternionNormalize(XMQuaternionMultiply(current, roll));
    XMStoreFloat4(&orientation, current);
    orientationAnimating = false;
}

void Camera::SetInput(const FlightInput& value)
{
    input = value;
}

void Camera::SeedOrbitInertia(float velocityX, float velocityY)
{
    inertiaX = std::clamp(static_cast<double>(velocityX), -kInertiaMaxPixelsPerSecond, kInertiaMaxPixelsPerSecond);
    inertiaY = std::clamp(static_cast<double>(velocityY), -kInertiaMaxPixelsPerSecond, kInertiaMaxPixelsPerSecond);
}

void Camera::SeedPanInertia(float velocityX, float velocityY)
{
    panInertiaX = std::clamp(static_cast<double>(velocityX), -kInertiaMaxPixelsPerSecond, kInertiaMaxPixelsPerSecond);
    panInertiaY = std::clamp(static_cast<double>(velocityY), -kInertiaMaxPixelsPerSecond, kInertiaMaxPixelsPerSecond);
}

void Camera::CancelInertia()
{
    inertiaX = 0.0;
    inertiaY = 0.0;
    panInertiaX = 0.0;
    panInertiaY = 0.0;
}

void Camera::StopMotion()
{
    velRight = 0.0;
    velUp = 0.0;
    velForward = 0.0;
    rollRate = 0.0;
    orbitRateX = 0.0;
    orbitRateY = 0.0;
    panRateX = 0.0;
    panRateY = 0.0;
    CancelInertia();
    // A stopped camera keeps its projection blend settled where it is; the
    // blend continues to ease toward the selected mode via Update().
}

double Camera::FlightSpeed() const
{
    // Fly at a rate that scales with the scene, and slow down as the camera
    // closes in on the subject for precision, faster when pulled back.
    const double base = std::max(sceneRadius * 1.25, 1e-9);
    const double zoomScale = std::clamp(distance / std::max(1e-9, sceneRadius * 2.0), 0.22, 2.8);
    return base * zoomScale * (input.fast ? kBoostMultiplier : 1.0) * flySpeedScale;
}

void Camera::Update(double deltaTime)
{
    // Apply any queued raw mouse-look first, so this tick's WASD translation
    // (below) moves along the orientation the player is looking at *now*
    // rather than the stale one from before this frame's mouse deltas.
    if (pendingLookX != 0.0 || pendingLookY != 0.0)
    {
        Look(static_cast<float>(pendingLookX), static_cast<float>(pendingLookY));
        pendingLookX = 0.0;
        pendingLookY = 0.0;
    }

    const double flightEase = EaseFactor(deltaTime, kFlightAccelSeconds);

    // Smoothed keyboard flight: velocity eases toward the intent instead of
    // toggling, so motion ramps up on press and settles on release.
    double directionRight = input.right;
    double directionUp = input.up;
    double directionForward = input.forward;
    const double directionLength = std::sqrt(directionRight * directionRight + directionUp * directionUp +
        directionForward * directionForward);
    if (directionLength > 1.0)
    {
        directionRight /= directionLength;
        directionUp /= directionLength;
        directionForward /= directionLength;
    }
    const double speed = FlightSpeed();
    const double stopEpsilon = std::max(speed * 0.01, 1e-9);
    velRight += (directionRight * speed - velRight) * flightEase;
    velUp += (directionUp * speed - velUp) * flightEase;
    velForward += (directionForward * speed - velForward) * flightEase;
    if (std::abs(velRight) < stopEpsilon) velRight = 0.0;
    if (std::abs(velUp) < stopEpsilon) velUp = 0.0;
    if (std::abs(velForward) < stopEpsilon) velForward = 0.0;
    if (velRight != 0.0 || velUp != 0.0 || velForward != 0.0)
    {
        MoveLocal(static_cast<float>(velRight * deltaTime), static_cast<float>(velUp * deltaTime),
            static_cast<float>(velForward * deltaTime));
    }

    const double rollTarget = input.roll * kRollBaseSpeed * (input.fast ? 2.0 : 1.0);
    rollRate += (rollTarget - rollRate) * EaseFactor(deltaTime, kRollAccelSeconds);
    if (std::abs(rollRate) < 0.01) rollRate = 0.0;
    if (rollRate != 0.0) Roll(static_cast<float>(rollRate * deltaTime));

    // Continuous, eased arrow-key orbit and Shift+arrow pan.
    orbitRateX += (static_cast<double>(input.orbitX) - orbitRateX) * flightEase;
    orbitRateY += (static_cast<double>(input.orbitY) - orbitRateY) * flightEase;
    if (std::abs(orbitRateX) < 25.0) orbitRateX = 0.0;
    if (std::abs(orbitRateY) < 25.0) orbitRateY = 0.0;
    if (orbitRateX != 0.0 || orbitRateY != 0.0)
    {
        ApplyOrbitAngles(-static_cast<float>(orbitRateX * deltaTime) * kOrbitPixelsToRadians,
            -static_cast<float>(orbitRateY * deltaTime) * kOrbitPixelsToRadians);
    }

    panRateX += (static_cast<double>(input.panX) - panRateX) * flightEase;
    panRateY += (static_cast<double>(input.panY) - panRateY) * flightEase;
    if (std::abs(panRateX) < 25.0) panRateX = 0.0;
    if (std::abs(panRateY) < 25.0) panRateY = 0.0;
    if (panRateX != 0.0 || panRateY != 0.0)
    {
        Truck(static_cast<float>(panRateX * deltaTime), static_cast<float>(panRateY * deltaTime), input.viewportHeight,
            /*axisSnap=*/false);
    }

    // Post-drag orbit inertia.
    if (inertiaX != 0.0 || inertiaY != 0.0)
    {
        ApplyOrbitAngles(-static_cast<float>(inertiaX * deltaTime) * kOrbitPixelsToRadians,
            -static_cast<float>(inertiaY * deltaTime) * kOrbitPixelsToRadians);
        const double decay = deltaTime > 0.0 ? std::exp(-deltaTime / kInertiaDecaySeconds) : 0.0;
        inertiaX *= decay;
        inertiaY *= decay;
        if (std::abs(inertiaX) < kInertiaMinPixelsPerSecond && std::abs(inertiaY) < kInertiaMinPixelsPerSecond)
        {
            CancelInertia();
        }
    }

    // Post-drag pan inertia, exponentially damped the same way.
    if (panInertiaX != 0.0 || panInertiaY != 0.0)
    {
        const double height = lastViewportHeight;
        if (height > 1.0)
        {
            Truck(static_cast<float>(panInertiaX * deltaTime), static_cast<float>(panInertiaY * deltaTime),
                static_cast<float>(height), /*axisSnap=*/false);
        }
        const double decay = deltaTime > 0.0 ? std::exp(-deltaTime / kInertiaDecaySeconds) : 0.0;
        panInertiaX *= decay;
        panInertiaY *= decay;
        if (std::abs(panInertiaX) < kInertiaMinPixelsPerSecond && std::abs(panInertiaY) < kInertiaMinPixelsPerSecond)
        {
            panInertiaX = 0.0;
            panInertiaY = 0.0;
        }
    }

    // Perspective <-> orthographic cross-fade. Blending the two projection
    // matrices per element reads as a short dolly-zoom instead of a hard cut.
    {
        const double projectionTarget = projection == ProjectionMode::Orthographic ? 1.0 : 0.0;
        if (projectionBlend != projectionTarget)
        {
            projectionBlend += (projectionTarget - projectionBlend) * EaseFactor(deltaTime, kProjectionBlendSeconds);
            if (std::abs(projectionBlend - projectionTarget) < 0.002) projectionBlend = projectionTarget;
        }
    }

    // Eased wheel zoom toward the target distance.
    if (distance != targetDistance)
    {
        distance += (targetDistance - distance) * EaseFactor(deltaTime, kZoomEaseSeconds);
        if (std::fabs(std::log(distance / targetDistance)) < 1e-3) distance = targetDistance;
    }

    // Fit glide: ease the orbit pivot back to the model center.
    if (pivotAnimating)
    {
        pivotAnimElapsed += deltaTime;
        const double t = std::min(1.0, pivotAnimElapsed / kPivotAnimSeconds);
        const double smooth = t * t * (3.0 - 2.0 * t);
        targetX = pivotFromX + (desiredX - pivotFromX) * smooth;
        targetY = pivotFromY + (desiredY - pivotFromY) * smooth;
        targetZ = pivotFromZ + (desiredZ - pivotFromZ) * smooth;
        if (t >= 1.0)
        {
            targetX = desiredX;
            targetY = desiredY;
            targetZ = desiredZ;
            pivotAnimating = false;
        }
    }

    // Reset glide: slerp the orientation back to the home view.
    if (orientationAnimating)
    {
        orientationAnimElapsed += deltaTime;
        const double t = std::min(1.0, orientationAnimElapsed / kOrientationAnimSeconds);
        const float smooth = static_cast<float>(t * t * (3.0 - 2.0 * t));
        const XMVECTOR from = XMLoadFloat4(&orientationFrom);
        const XMVECTOR to = XMQuaternionNormalize(XMLoadFloat4(&desiredOrientation));
        // Take the shortest arc: flip the target when its sign points away.
        const float alignment = XMVectorGetX(XMVector4Dot(from, to));
        const XMVECTOR shortest = alignment < 0.0f ? XMVectorNegate(to) : to;
        XMVECTOR next = XMQuaternionNormalize(XMQuaternionSlerp(from, shortest, smooth));
        if (t >= 1.0)
        {
            next = to;
            orientationAnimating = false;
        }
        XMStoreFloat4(&orientation, next);
    }
}

bool Camera::HasMotion() const
{
    if (pendingLookX != 0.0 || pendingLookY != 0.0) return true;
    if (velRight != 0.0 || velUp != 0.0 || velForward != 0.0) return true;
    if (rollRate != 0.0) return true;
    if (orbitRateX != 0.0 || orbitRateY != 0.0) return true;
    if (panRateX != 0.0 || panRateY != 0.0) return true;
    if (inertiaX != 0.0 || inertiaY != 0.0) return true;
    if (panInertiaX != 0.0 || panInertiaY != 0.0) return true;
    if (pivotAnimating || orientationAnimating) return true;
    if (distance != targetDistance) return true;
    if (projectionBlend != (projection == ProjectionMode::Orthographic ? 1.0 : 0.0)) return true;
    return false;
}

XMVECTOR Camera::Orientation() const
{
    return LoadOrientation(*this);
}

XMVECTOR Camera::EyePosition() const
{
    const XMVECTOR pivot = XMVectorSet(static_cast<float>(targetX), static_cast<float>(targetY),
        static_cast<float>(targetZ), 1.0f);
    return pivot + XMVector3Rotate(XMVectorSet(0, 0, static_cast<float>(distance), 0), Orientation());
}

double Camera::FarBound() const
{
    // Robust far plane for panned/flown-away cameras: the eye-to-scene-center
    // distance is bounded by the pivot distance plus the pivot's drift from
    // the scene's cached home center.
    const double driftX = targetX - homeX;
    const double driftY = targetY - homeY;
    const double driftZ = targetZ - homeZ;
    const double drift = std::sqrt(driftX * driftX + driftY * driftY + driftZ * driftZ);
    return distance + drift + sceneRadius * 8.0;
}

XMMATRIX Camera::ViewMatrix() const
{
    const XMVECTOR eye = EyePosition();
    const XMVECTOR pivot = XMVectorSet(static_cast<float>(targetX), static_cast<float>(targetY),
        static_cast<float>(targetZ), 1.0f);
    // The camera's own up is always perpendicular to its forward by
    // construction (yaw/pitch compose about the camera's own axes), so the
    // look-at never degenerates; the guard covers bit-level edge cases only.
    XMVECTOR up = XMVector3Rotate(XMVectorSet(0, 1, 0, 0), Orientation());
    const XMVECTOR viewAxis = XMVector3Normalize(XMVectorSubtract(eye, pivot));
    if (std::abs(XMVectorGetX(XMVector3Dot(XMVector3Normalize(up), viewAxis))) > 0.999999f)
    {
        up = XMVectorSet(0, 0, 1, 0);
    }
    return XMMatrixLookAtRH(eye, pivot, up);
}

XMMATRIX Camera::ProjectionMatrix(float aspect) const
{
    aspect = std::max(0.05f, aspect);
    const float nearPlane = static_cast<float>(std::max(sceneRadius * 0.00001, distance * 0.0005));
    const float farPlane = static_cast<float>(std::max(FarBound(), static_cast<double>(nearPlane) * 100.0));
    const XMMATRIX perspective = XMMatrixPerspectiveFovRH(kVerticalFieldOfView, aspect, nearPlane, farPlane);
    if (projectionBlend <= 0.0) return perspective;
    // Orthographic half-height matches the perspective frustum height at the
    // pivot distance, so the toggle preserves framing and dolly stays unified.
    const float halfHeight = static_cast<float>(distance * std::tan(kVerticalFieldOfView * 0.5));
    const XMMATRIX orthographic = XMMatrixOrthographicRH(halfHeight * aspect * 2.0f, halfHeight * 2.0f,
        nearPlane, farPlane);
    if (projectionBlend >= 1.0) return orthographic;
    const float t = static_cast<float>(projectionBlend);
    const float smooth = t * t * (3.0f - 2.0f * t);
    XMMATRIX blended{};
    for (int row = 0; row < 4; ++row)
    {
        blended.r[row] = XMVectorLerp(perspective.r[row], orthographic.r[row], smooth);
    }
    return blended;
}

RECT CalculateErrorCardRect(int width, int height, int toolbarHeight, float dpiScale)
{
    const int cardWidth = std::min(static_cast<int>(Scale(560.0f, dpiScale)), std::max(280, width - static_cast<int>(Scale(32.0f, dpiScale))));
    const int cardHeight = std::min(static_cast<int>(Scale(258.0f, dpiScale)), std::max(220, height - toolbarHeight - static_cast<int>(Scale(32.0f, dpiScale))));
    const int left = (width - cardWidth) / 2;
    const int top = toolbarHeight + std::max(0, (height - toolbarHeight - cardHeight) / 2);
    return RECT{ left, top, left + cardWidth, top + cardHeight };
}

struct Renderer::Impl
{
    HWND window = nullptr;
    int width = 0;
    int height = 0;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<ID3D11RenderTargetView> renderTarget;
    ComPtr<ID3D11Texture2D> depthTexture;
    ComPtr<ID3D11DepthStencilView> depthView;
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11InputLayout> inputLayout;
    ComPtr<ID3D11Buffer> constantBuffer;
    ComPtr<ID3D11RasterizerState> rasterizerState;
    ComPtr<ID3D11DepthStencilState> depthState;
    ComPtr<ID3D11BlendState> blendState;
    ComPtr<ID3D11Buffer> vertexBuffer;
    ComPtr<ID3D11Buffer> indexBuffer;
    ComPtr<ID3D11Buffer> gridBuffer;
    UINT indexCount = 0;
    UINT gridVertexCount = 0;
    XMFLOAT3 boundsMin{};
    XMFLOAT3 boundsMax{};

    ComPtr<ID2D1Factory> d2dFactory;
    ComPtr<IDWriteFactory> writeFactory;
    ComPtr<ID2D1RenderTarget> overlayTarget;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1StrokeStyle> dashedStroke;
    ComPtr<ID2D1StrokeStyle> spinnerStroke;
    ComPtr<IDWriteTextFormat> headingFormat;
    ComPtr<IDWriteTextFormat> bodyFormat;
    ComPtr<IDWriteTextFormat> smallFormat;
    ComPtr<IDWriteTextFormat> filenameFormat;
    ComPtr<IDWriteTextFormat> gizmoFormat;
    float textScale = 0.0f;

    bool CreateTargets(std::wstring& error)
    {
        ComPtr<ID3D11Texture2D> backBuffer;
        HRESULT result = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (FAILED(result) || FAILED(device->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget)))
        {
            error = L"The window render target could not be created.";
            return false;
        }

        D3D11_TEXTURE2D_DESC depthDescription{};
        depthDescription.Width = static_cast<UINT>(std::max(1, width));
        depthDescription.Height = static_cast<UINT>(std::max(1, height));
        depthDescription.MipLevels = 1;
        depthDescription.ArraySize = 1;
        depthDescription.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthDescription.SampleDesc.Count = 1;
        depthDescription.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(device->CreateTexture2D(&depthDescription, nullptr, &depthTexture)) ||
            FAILED(device->CreateDepthStencilView(depthTexture.Get(), nullptr, &depthView)))
        {
            error = L"The depth buffer could not be created.";
            return false;
        }

        ComPtr<IDXGISurface> surface;
        if (FAILED(backBuffer.As(&surface)))
        {
            error = L"The window surface is not compatible with Direct2D.";
            return false;
        }
        const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        if (FAILED(d2dFactory->CreateDxgiSurfaceRenderTarget(surface.Get(), &properties, &overlayTarget)) ||
            FAILED(overlayTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush)))
        {
            error = L"The text and overlay surface could not be created.";
            return false;
        }
        return true;
    }

    bool CreateTextFormats(float scale)
    {
        if (std::abs(textScale - scale) < 0.01f && bodyFormat) return true;
        headingFormat.Reset(); bodyFormat.Reset(); smallFormat.Reset(); filenameFormat.Reset();
        gizmoFormat.Reset();
        textScale = scale;
        auto create = [&](float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& output)
        {
            return SUCCEEDED(writeFactory->CreateTextFormat(L"Segoe UI Variable Text", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, Scale(size, scale), L"en-us", &output));
        };
        if (!create(22, DWRITE_FONT_WEIGHT_SEMI_BOLD, headingFormat) ||
            !create(14, DWRITE_FONT_WEIGHT_NORMAL, bodyFormat) ||
            !create(12, DWRITE_FONT_WEIGHT_NORMAL, smallFormat) ||
            !create(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, filenameFormat) ||
            !create(10, DWRITE_FONT_WEIGHT_SEMI_BOLD, gizmoFormat)) return false;
        headingFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        headingFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        filenameFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        smallFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        gizmoFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        gizmoFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        return true;
    }

    void SetBrush(D2D1_COLOR_F color)
    {
        brush->SetColor(color);
    }

    void DrawText(const std::wstring& text, IDWriteTextFormat* format, D2D1_RECT_F rectangle,
        D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING)
    {
        if (text.empty()) return;
        format->SetTextAlignment(alignment);
        SetBrush(color);
        overlayTarget->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rectangle, brush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void DrawGizmo(const Camera& camera, const NavGizmo& gizmo, float scale)
    {
        if (!gizmoFormat) return;
        const NavGizmo::DrawGeometry g = gizmo.ComputeDraw(camera.Orientation());
        const D2D1_POINT_2F center{ g.centerX, g.centerY };
        const D2D1_COLOR_F axisColors[3] = {
            D2D1::ColorF(0.98f, 0.28f, 0.32f, 1.0f),   // X red
            D2D1::ColorF(0.30f, 0.84f, 0.18f, 1.0f),   // Y green
            D2D1::ColorF(0.24f, 0.57f, 1.00f, 1.0f) }; // Z blue
        const wchar_t axisLetters[3] = { L'X', L'Y', L'Z' };

        // Ball: a quiet disc that brightens when the orbit-drag target.
        const bool ballHover = g.hover == NavGizmo::Part::Ball;
        SetBrush(D2D1::ColorF(0x11141A, ballHover ? 0.32f : 0.16f));
        overlayTarget->FillEllipse(D2D1::Ellipse(center, g.outerRadius, g.outerRadius), brush.Get());
        SetBrush(D2D1::ColorF(0xB9B9C2, ballHover ? 0.95f : 0.40f));
        overlayTarget->DrawEllipse(D2D1::Ellipse(center, g.outerRadius, g.outerRadius), brush.Get(), Scale(1.1f, scale));

        // Stems, dimmed when their axis points away from the viewer.
        for (int axis = 0; axis < 3; ++axis)
        {
            const float depth = (g.positive[axis].depth + g.negative[axis].depth) * 0.5f;
            const bool hovered = g.hover == static_cast<NavGizmo::Part>(
                static_cast<int>(NavGizmo::Part::PosX) + axis);
            const float alpha = depth >= 0.0f ? 0.95f : (hovered ? 0.75f : 0.40f);
            SetBrush(D2D1::ColorF(axisColors[axis].r, axisColors[axis].g, axisColors[axis].b, alpha));
            overlayTarget->DrawLine(center,
                D2D1::Point2F(center.x + g.positive[axis].x, center.y + g.positive[axis].y),
                brush.Get(), g.stemWidth);
        }

        // Nodes and dots, painted back to front by view-space depth so the
        // gizmo reads as a small 3D object rather than a flat diagram.
        struct NodePaint
        {
            const NavGizmo::NodeGeometry* node;
            int axis;
            bool positive;
        };
        NodePaint nodes[6]{};
        for (int axis = 0; axis < 3; ++axis)
        {
            nodes[axis] = { &g.positive[axis], axis, true };
            nodes[axis + 3] = { &g.negative[axis], axis, false };
        }
        std::sort(nodes, nodes + 6, [](const NodePaint& a, const NodePaint& b)
            { return a.node->depth < b.node->depth; });
        for (const NodePaint& node : nodes)
        {
            const float radius = node.positive ? g.nodeRadius : g.dotRadius;
            const D2D1_POINT_2F position{ center.x + node.node->x, center.y + node.node->y };
            const NavGizmo::Part part = static_cast<NavGizmo::Part>(
                static_cast<int>(NavGizmo::Part::PosX) + node.axis + (node.positive ? 0 : 3));
            const bool hovered = g.hover == part;
            const bool behind = node.node->depth < 0.0f;
            const D2D1_COLOR_F base = axisColors[node.axis];
            const float dim = behind && !hovered ? 0.55f : 1.0f;
            const float lift = hovered ? 0.35f : 0.0f;
            SetBrush(D2D1::ColorF(
                std::min(1.0f, base.r * dim + lift),
                std::min(1.0f, base.g * dim + lift),
                std::min(1.0f, base.b * dim + lift),
                behind && !hovered ? 0.60f : 1.0f));
            overlayTarget->FillEllipse(D2D1::Ellipse(position, radius, radius), brush.Get());
            if (hovered)
            {
                SetBrush(D2D1::ColorF(0xFFFFFF, 0.95f));
                overlayTarget->DrawEllipse(D2D1::Ellipse(position, radius, radius), brush.Get(), Scale(1.6f, scale));
            }
            if (node.positive)
            {
                const std::wstring letter(1, axisLetters[node.axis]);
                SetBrush(D2D1::ColorF(0xFFFFFF, behind ? 0.80f : 1.0f));
                overlayTarget->DrawTextW(letter.c_str(), static_cast<UINT32>(letter.size()), gizmoFormat.Get(),
                    D2D1::RectF(position.x - radius, position.y - radius, position.x + radius, position.y + radius),
                    brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
    }

    // Photos-style bottom bar: a bar strip matching the title bar's fill,
    // with the Info button docked at the far left, a D2D-drawn zoom slider
    // (ZoomTrackRect, Preview3D.cpp — same split as the Speed flyout track)
    // docked bottom-right next to the zoom-percent readout, and the
    // Fullscreen toggle at the very right. Only drawn while a model is loaded
    // — see OverlayInfo::barBottomBarHeight, which the app zeroes out
    // otherwise. All three button rects are computed by Preview3D.cpp (same
    // split as the zoom track) so hit-testing and drawing never drift apart.
    // Drawn (as a floating toolbar over the viewport, slightly translucent)
    // even in Fullscreen, where OverlayInfo::bottomBarHeight — the *reserved*
    // viewport inset — has already collapsed to 0; barBottomBarHeight is the
    // bar's own always-real height, so it keeps showing (and the Fullscreen
    // toggle stays reachable) instead of vanishing along with that space.
    void DrawBottomBar(const OverlayInfo& overlay, float clientWidth, float clientHeight, float scale)
    {
        if (overlay.barBottomBarHeight <= 0) return;
        const float barTop = clientHeight - static_cast<float>(overlay.barBottomBarHeight);
        const float fillAlpha = overlay.isFullscreen ? 0.88f : 1.0f;
        SetBrush(D2D1::ColorF(0x2C2C2E, fillAlpha));
        overlayTarget->FillRectangle(D2D1::RectF(0, barTop, clientWidth, clientHeight), brush.Get());
        SetBrush(D2D1::ColorF(0x3A3A3C));
        overlayTarget->FillRectangle(D2D1::RectF(0, barTop, clientWidth, barTop + 1.0f), brush.Get());

        DrawIconButton(overlay.infoButtonRect, IconKind::Info, /*visible*/ true, /*enabled*/ true,
            overlay.infoPanelVisible, overlay.infoButtonHover, overlay.infoButtonPressed, scale);

        const std::wstring percentText = std::to_wstring(static_cast<int>(std::lround(overlay.zoomPercent))) + L"%";
        const float labelWidth = Scale(56, scale);
        const float gap = Scale(10, scale);
        const D2D1_RECT_F fullscreenRect = ToRectF(overlay.fullscreenButtonRect);
        DrawText(percentText, smallFormat.Get(),
            D2D1::RectF(fullscreenRect.left - gap - labelWidth, barTop, fullscreenRect.left - gap, clientHeight),
            D2D1::ColorF(0xA1A1A6), DWRITE_TEXT_ALIGNMENT_TRAILING);

        const D2D1_RECT_F track = ToRectF(overlay.zoomTrackRect);
        const float trackY = (track.top + track.bottom) * 0.5f;
        SetBrush(D2D1::ColorF(0x48484C));
        overlayTarget->DrawLine(D2D1::Point2F(track.left, trackY), D2D1::Point2F(track.right, trackY), brush.Get(), Scale(3, scale));
        const float thumbX = track.left + (track.right - track.left) * std::clamp(overlay.zoomSliderT, 0.0f, 1.0f);
        SetBrush(D2D1::ColorF(0x0A84FF));
        overlayTarget->DrawLine(D2D1::Point2F(track.left, trackY), D2D1::Point2F(thumbX, trackY), brush.Get(), Scale(3, scale));
        SetBrush(D2D1::ColorF(0xF5F5F7));
        overlayTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, trackY), Scale(7, scale), Scale(7, scale)), brush.Get());

        DrawIconButton(overlay.fullscreenButtonRect, overlay.isFullscreen ? IconKind::FullscreenExit : IconKind::FullscreenEnter,
            /*visible*/ true, /*enabled*/ true, overlay.isFullscreen, overlay.fullscreenButtonHover, overlay.fullscreenButtonPressed, scale);
    }

    // Shared icon-button chrome (fill/hover/press/active/disabled) for both
    // the title-bar action buttons and the bottom-bar Info/Fullscreen
    // buttons, painting one of the vector glyphs from IconKind on top.
    void DrawIconButton(const RECT& rectI, IconKind icon, bool visible, bool enabled, bool active,
        bool hovered, bool pressedNow, float scale)
    {
        if (!visible) return;
        const D2D1_RECT_F rect = ToRectF(rectI);
        D2D1_COLOR_F fill = D2D1::ColorF(0x3A3A3C);
        D2D1_COLOR_F iconColor = D2D1::ColorF(0xF5F5F7);
        if (active) fill = D2D1::ColorF(0x0A84FF, 0.28f);
        if (hovered) fill = active ? D2D1::ColorF(0x0A84FF, 0.40f) : D2D1::ColorF(0x46464A);
        if (pressedNow) fill = active ? D2D1::ColorF(0x0A84FF, 0.55f) : D2D1::ColorF(0x2C2C2E);
        if (!enabled) { fill = D2D1::ColorF(0x2C2C2E); iconColor = D2D1::ColorF(0x707075); }
        SetBrush(fill);
        overlayTarget->FillRoundedRectangle(D2D1::RoundedRect(rect, Scale(6, scale), Scale(6, scale)), brush.Get());
        SetBrush(iconColor);
        DrawIcon(overlayTarget.Get(), brush.Get(), icon, rect, scale);
    }

    // Right-docked, read-only "Stats & Shading" panel — see InfoPanel.h for
    // the section/row content, built by the app from the loaded model's
    // scanned ModelStats.
    void DrawInfoPanel(const OverlayInfo& overlay, float clientWidth, float clientHeight, float scale)
    {
        if (overlay.infoPanelWidth <= 0) return;
        const float panelWidth = static_cast<float>(overlay.infoPanelWidth);
        const float left = clientWidth - panelWidth;
        const float top = static_cast<float>(overlay.barToolbarHeight);
        const float bottom = clientHeight - static_cast<float>(overlay.barBottomBarHeight);

        SetBrush(D2D1::ColorF(0x242426));
        overlayTarget->FillRectangle(D2D1::RectF(left, top, clientWidth, bottom), brush.Get());
        SetBrush(D2D1::ColorF(0x3A3A3C));
        overlayTarget->FillRectangle(D2D1::RectF(left, top, left + 1.0f, bottom), brush.Get());

        const float margin = Scale(16, scale);
        const float rowHeight = Scale(24, scale);
        const float sectionGap = Scale(18, scale);
        const float textLeft = left + margin;
        const float textRight = clientWidth - margin;
        float y = top + Scale(18, scale);

        DrawText(L"Stats & Shading", filenameFormat.Get(),
            D2D1::RectF(textLeft, y, textRight, y + Scale(22, scale)), D2D1::ColorF(0xF5F5F7));
        y += Scale(34, scale);

        for (const InfoPanelSection& section : overlay.infoPanelSections)
        {
            if (y > bottom) break;
            DrawText(section.title, smallFormat.Get(), D2D1::RectF(textLeft, y, textRight, y + rowHeight),
                D2D1::ColorF(0x0A84FF));
            y += rowHeight;
            for (const InfoPanelRow& row : section.rows)
            {
                if (y > bottom) break;
                const D2D1_RECT_F rowRect = D2D1::RectF(textLeft, y, textRight, y + rowHeight);
                DrawText(row.label, smallFormat.Get(), rowRect, D2D1::ColorF(0xA1A1A6));
                DrawText(row.value, smallFormat.Get(), rowRect, D2D1::ColorF(0xF5F5F7), DWRITE_TEXT_ALIGNMENT_TRAILING);
                y += rowHeight;
            }
            y += sectionGap - rowHeight;
        }
    }

    static D2D1_RECT_F ToRectF(RECT rect)
    {
        return D2D1::RectF(static_cast<float>(rect.left), static_cast<float>(rect.top),
            static_cast<float>(rect.right), static_cast<float>(rect.bottom));
    }

    // Windows-11-Photos-style unified title bar: action buttons, centered
    // filename, Open With, and the (D2D-drawn, NC-hit-tested) system
    // min/max/close — all laid out by Chrome::UpdateLayout.
    void DrawTitleBar(const OverlayInfo& overlay, const Chrome& chrome, float clientWidth, float scale)
    {
        const RECT barRectI = chrome.TitleBarRect();
        const float barHeight = static_cast<float>(barRectI.bottom);
        // Fullscreen: same strip, drawn slightly translucent (matching
        // DrawBottomBar) so it reads as a floating toolbar over the viewport
        // rather than the fully opaque, space-reserving windowed title bar.
        SetBrush(D2D1::ColorF(0x2C2C2E, overlay.isFullscreen ? 0.88f : 1.0f));
        overlayTarget->FillRectangle(D2D1::RectF(0, 0, clientWidth, barHeight), brush.Get());
        SetBrush(D2D1::ColorF(0x3A3A3C));
        overlayTarget->FillRectangle(D2D1::RectF(0, barHeight - 1, clientWidth, barHeight), brush.Get());

        const std::wstring filename = overlay.filename.empty() ? L"3D Preview" : overlay.filename;
        DrawText(filename, filenameFormat.Get(), ToRectF(chrome.FilenameRect()), D2D1::ColorF(0xF5F5F7),
            DWRITE_TEXT_ALIGNMENT_CENTER);

        // Icon-only action buttons (vector glyphs, IconKind/DrawIcon above),
        // matching the old owner-drawn toolbar button palette
        // (primary/secondary/active/hover/pressed).
        auto drawActionButton = [&](Chrome::Part part, IconKind icon, bool active)
        {
            const Chrome::ButtonState& state = chrome.Button(part);
            const bool hovered = chrome.hover == part;
            const bool pressedNow = chrome.pressed == part;
            DrawIconButton(state.rect, icon, state.visible, state.enabled, active, hovered, pressedNow, scale);
        };
        drawActionButton(Chrome::Part::Grid, IconKind::Grid, overlay.gridVisible);
        drawActionButton(Chrome::Part::AxisSnap, IconKind::AxisSnap, overlay.axisSnapEnabled);
        drawActionButton(Chrome::Part::Speed, IconKind::Speed, false);
        drawActionButton(Chrome::Part::Fit, IconKind::Fit, false);
        drawActionButton(Chrome::Part::Reset, IconKind::Reset, false);
        drawActionButton(Chrome::Part::Share, IconKind::Share, false);
        drawActionButton(Chrome::Part::Overflow, IconKind::Overflow, false);
        drawActionButton(Chrome::Part::OpenWith, IconKind::OpenWith, false);

        // System caption buttons: simple vector glyphs, matching Windows 11's
        // minimize/maximize-or-restore/close. DWM (via DwmDefWindowProc,
        // Preview3D.cpp) draws the hover/press background for these — the
        // hover/press tint here is a same-frame fallback for any gap before
        // that first paints.
        auto drawSystemButton = [&](Chrome::Part part, auto drawGlyph, bool closeButton)
        {
            const Chrome::ButtonState& state = chrome.Button(part);
            if (!state.visible) return;
            const D2D1_RECT_F rect = ToRectF(state.rect);
            const bool hovered = chrome.hover == part;
            const bool pressedNow = chrome.pressed == part;
            if (pressedNow) { SetBrush(closeButton ? D2D1::ColorF(0xC42B1C) : D2D1::ColorF(0x3F3F42)); overlayTarget->FillRectangle(rect, brush.Get()); }
            else if (hovered) { SetBrush(closeButton ? D2D1::ColorF(0xE81123) : D2D1::ColorF(0x35353A)); overlayTarget->FillRectangle(rect, brush.Get()); }
            SetBrush(D2D1::ColorF(0xF5F5F7));
            const float cx = (rect.left + rect.right) * 0.5f;
            const float cy = (rect.top + rect.bottom) * 0.5f;
            drawGlyph(cx, cy);
        };
        const float glyphHalf = Scale(5, scale);
        drawSystemButton(Chrome::Part::Minimize, [&](float cx, float cy)
        {
            overlayTarget->DrawLine(D2D1::Point2F(cx - glyphHalf, cy), D2D1::Point2F(cx + glyphHalf, cy), brush.Get(), 1.0f);
        }, false);
        drawSystemButton(Chrome::Part::Maximize, [&](float cx, float cy)
        {
            if (chrome.Maximized())
            {
                const float inset = Scale(2, scale);
                overlayTarget->DrawRectangle(D2D1::RectF(cx - glyphHalf + inset, cy - glyphHalf, cx + glyphHalf, cy + glyphHalf - inset), brush.Get(), 1.0f);
                overlayTarget->DrawRectangle(D2D1::RectF(cx - glyphHalf, cy - glyphHalf + inset, cx + glyphHalf - inset, cy + glyphHalf), brush.Get(), 1.0f);
            }
            else
            {
                overlayTarget->DrawRectangle(D2D1::RectF(cx - glyphHalf, cy - glyphHalf, cx + glyphHalf, cy + glyphHalf), brush.Get(), 1.0f);
            }
        }, false);
        drawSystemButton(Chrome::Part::Close, [&](float cx, float cy)
        {
            overlayTarget->DrawLine(D2D1::Point2F(cx - glyphHalf, cy - glyphHalf), D2D1::Point2F(cx + glyphHalf, cy + glyphHalf), brush.Get(), 1.0f);
            overlayTarget->DrawLine(D2D1::Point2F(cx - glyphHalf, cy + glyphHalf), D2D1::Point2F(cx + glyphHalf, cy - glyphHalf), brush.Get(), 1.0f);
        }, true);
    }

    // Photos-style "drop-down under the button with a slider and a number
    // value" for travel speed — a small floating panel below the Speed
    // button, drawn on top of the viewport. The app owns the panel/track
    // rects and drag math (Preview3D.cpp); this only draws them.
    void DrawSpeedFlyout(const OverlayInfo& overlay, float scale)
    {
        if (!overlay.speedFlyoutOpen) return;
        const D2D1_RECT_F panel = ToRectF(overlay.speedFlyoutRect);
        SetBrush(D2D1::ColorF(0x242426, 0.98f));
        overlayTarget->FillRoundedRectangle(D2D1::RoundedRect(panel, Scale(10, scale), Scale(10, scale)), brush.Get());
        SetBrush(D2D1::ColorF(0x3A3A3C));
        overlayTarget->DrawRoundedRectangle(D2D1::RoundedRect(panel, Scale(10, scale), Scale(10, scale)), brush.Get(), 1.0f);

        DrawText(L"Travel speed", smallFormat.Get(),
            D2D1::RectF(panel.left + Scale(16, scale), panel.top + Scale(8, scale), panel.right - Scale(16, scale), panel.top + Scale(26, scale)),
            D2D1::ColorF(0xA1A1A6));
        DrawText(overlay.speedValueText, smallFormat.Get(),
            D2D1::RectF(panel.left + Scale(16, scale), panel.top + Scale(8, scale), panel.right - Scale(16, scale), panel.top + Scale(26, scale)),
            D2D1::ColorF(0xF5F5F7), DWRITE_TEXT_ALIGNMENT_TRAILING);

        const D2D1_RECT_F track = ToRectF(overlay.speedFlyoutTrackRect);
        const float trackY = (track.top + track.bottom) * 0.5f;
        SetBrush(D2D1::ColorF(0x48484C));
        overlayTarget->DrawLine(D2D1::Point2F(track.left, trackY), D2D1::Point2F(track.right, trackY), brush.Get(), Scale(3, scale));
        const float thumbX = track.left + (track.right - track.left) * std::clamp(overlay.speedSliderT, 0.0f, 1.0f);
        SetBrush(D2D1::ColorF(0x0A84FF));
        overlayTarget->DrawLine(D2D1::Point2F(track.left, trackY), D2D1::Point2F(thumbX, trackY), brush.Get(), Scale(3, scale));
        SetBrush(D2D1::ColorF(0xF5F5F7));
        overlayTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, trackY), Scale(7, scale), Scale(7, scale)), brush.Get());
    }

    // A small dark bubble naming the button under the cursor, shown once
    // Preview3D.cpp's hover-delay timer decides the pointer has parked on a
    // button for a while (see ComputeTooltipInfo). Anchored below title-bar
    // buttons and above bottom-bar ones (overlay.tooltipBelow) so it never
    // reads as covering the button it describes.
    void DrawTooltip(const OverlayInfo& overlay, float clientWidth, float scale)
    {
        if (!overlay.tooltipVisible || overlay.tooltipText.empty() || !smallFormat) return;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(writeFactory->CreateTextLayout(overlay.tooltipText.c_str(), static_cast<UINT32>(overlay.tooltipText.size()),
            smallFormat.Get(), Scale(300, scale), Scale(200, scale), &layout))) return;
        // smallFormat is shared and mutated in place by every other DrawText()
        // call this frame (e.g. DrawBottomBar's TRAILING-aligned percent
        // readout, drawn just before this) — pin this layout's own alignment
        // so it never inherits whatever that last call left behind (with a
        // wide layout box, TRAILING alignment pushed the text off past the
        // right edge of the bubble, making it silently invisible).
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(layout->GetMetrics(&metrics))) return;

        const float paddingX = Scale(10, scale);
        const float paddingY = Scale(6, scale);
        const float gap = Scale(8, scale);
        const float margin = Scale(4, scale);
        const float bubbleWidth = metrics.width + paddingX * 2.0f;
        const float bubbleHeight = metrics.height + paddingY * 2.0f;

        const D2D1_RECT_F anchor = ToRectF(overlay.tooltipAnchorRect);
        const float anchorCenterX = (anchor.left + anchor.right) * 0.5f;
        const float left = std::clamp(anchorCenterX - bubbleWidth * 0.5f, margin, std::max(margin, clientWidth - bubbleWidth - margin));
        const float top = overlay.tooltipBelow ? anchor.bottom + gap : anchor.top - gap - bubbleHeight;
        const D2D1_RECT_F bubble = D2D1::RectF(left, top, left + bubbleWidth, top + bubbleHeight);

        SetBrush(D2D1::ColorF(0x1C1C1E, 0.97f));
        overlayTarget->FillRoundedRectangle(D2D1::RoundedRect(bubble, Scale(5, scale), Scale(5, scale)), brush.Get());
        SetBrush(D2D1::ColorF(0x48484C));
        overlayTarget->DrawRoundedRectangle(D2D1::RoundedRect(bubble, Scale(5, scale), Scale(5, scale)), brush.Get(), 1.0f);
        SetBrush(D2D1::ColorF(0xF5F5F7));
        overlayTarget->DrawTextLayout(D2D1::Point2F(left + paddingX, top + paddingY), layout.Get(), brush.Get());
    }

    void DrawOverlay(const Camera& camera, const OverlayInfo& overlay, const NavGizmo& gizmo, const Chrome& chrome)
    {
        if (!overlayTarget || !brush) return;
        if (!CreateTextFormats(overlay.dpiScale)) return;
        const float scale = overlay.dpiScale;
        const float toolbar = static_cast<float>(overlay.barToolbarHeight);
        const float clientWidth = static_cast<float>(width);
        const float clientHeight = static_cast<float>(height);
        // Bottom-anchored chrome (status pill, warning badge, HUD pills) sits
        // above the bottom bar and left of the Information panel, both of
        // which are only reserved while a model is loaded (see
        // OverlayInfo::barBottomBarHeight/infoPanelWidth).
        const float contentBottom = clientHeight - static_cast<float>(overlay.barBottomBarHeight);
        const float contentRight = clientWidth - static_cast<float>(overlay.infoPanelWidth);

        overlayTarget->BeginDraw();
        // Drawn first (so the system caption buttons never disappear while a
        // model is loading — this used to be skipped entirely during
        // ViewerState::Loading, which is what made the window briefly
        // uncontrollable right after launching with a file or dropping one
        // in) — including in Fullscreen, where it becomes a floating toolbar
        // (Chrome::UpdateLayout hides Minimize/Maximize/Close/Open With there
        // instead, keeping just the action buttons) drawn over the
        // full-monitor viewport rather than pushed out of it: Preview3D.cpp's
        // EffectiveToolbarHeight still collapses the *reserved* space to 0 in
        // that state and bypasses the title bar's NC hit-testing to match, so
        // the viewport fills the whole window with no dead strip of chrome
        // reserved at the top, while OverlayInfo::barToolbarHeight (`toolbar`
        // above) keeps its real value so the bar still draws and its buttons
        // still hit-test as ordinary client-area ones.
        DrawTitleBar(overlay, chrome, clientWidth, scale);
        if (overlay.state == ViewerState::Loading)
        {
            const float centerX = clientWidth * 0.5f;
            const float centerY = clientHeight * 0.5f;
            constexpr int spokeCount = 12;
            const int leadingSpoke = static_cast<int>(overlay.animationPhase * spokeCount) % spokeCount;
            for (int spoke = 0; spoke < spokeCount; ++spoke)
            {
                const float angle = static_cast<float>(spoke) / spokeCount * XM_2PI - XM_PIDIV2;
                const int age = (spoke - leadingSpoke + spokeCount) % spokeCount;
                const float alpha = 0.14f + 0.78f * (1.0f - static_cast<float>(age) / spokeCount);
                const float innerRadius = Scale(10.0f, scale);
                const float outerRadius = Scale(17.0f, scale);
                SetBrush(D2D1::ColorF(0xF5F5F7, alpha));
                overlayTarget->DrawLine(
                    D2D1::Point2F(centerX + std::cos(angle) * innerRadius, centerY + std::sin(angle) * innerRadius),
                    D2D1::Point2F(centerX + std::cos(angle) * outerRadius, centerY + std::sin(angle) * outerRadius),
                    brush.Get(), Scale(2.4f, scale), spinnerStroke.Get());
            }
            if (overlayTarget->EndDraw() == D2DERR_RECREATE_TARGET)
            {
                overlayTarget.Reset();
                brush.Reset();
            }
            return;
        }

        const D2D1_COLOR_F primaryText = D2D1::ColorF(0xF5F5F7);
        const D2D1_COLOR_F secondaryText = D2D1::ColorF(0xA1A1A6);

        if (overlay.state == ViewerState::Empty)
        {
            const float centerX = clientWidth * 0.5f;
            const float centerY = toolbar + (clientHeight - toolbar) * 0.48f;
            const float cardWidth = std::min(Scale(520, scale), clientWidth - Scale(36, scale));
            const float cardHeight = Scale(230, scale);
            const D2D1_ROUNDED_RECT card = D2D1::RoundedRect(
                D2D1::RectF(centerX - cardWidth * 0.5f, centerY - cardHeight * 0.5f,
                    centerX + cardWidth * 0.5f, centerY + cardHeight * 0.5f), Scale(14, scale), Scale(14, scale));
            SetBrush(D2D1::ColorF(0x242426, 0.92f));
            overlayTarget->FillRoundedRectangle(card, brush.Get());
            SetBrush(D2D1::ColorF(0x3A3A3C));
            overlayTarget->DrawRoundedRectangle(card, brush.Get(), Scale(1, scale), dashedStroke.Get());

            SetBrush(D2D1::ColorF(0x0A84FF));
            overlayTarget->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(centerX, centerY - Scale(53, scale)),
                Scale(22, scale), Scale(22, scale)), brush.Get(), Scale(2, scale));
            overlayTarget->DrawLine(D2D1::Point2F(centerX, centerY - Scale(64, scale)),
                D2D1::Point2F(centerX, centerY - Scale(42, scale)), brush.Get(), Scale(2, scale));
            overlayTarget->DrawLine(D2D1::Point2F(centerX - Scale(7, scale), centerY - Scale(49, scale)),
                D2D1::Point2F(centerX, centerY - Scale(42, scale)), brush.Get(), Scale(2, scale));
            overlayTarget->DrawLine(D2D1::Point2F(centerX + Scale(7, scale), centerY - Scale(49, scale)),
                D2D1::Point2F(centerX, centerY - Scale(42, scale)), brush.Get(), Scale(2, scale));

            DrawText(L"Drop a GLB model here", headingFormat.Get(), D2D1::RectF(centerX - cardWidth * 0.45f,
                centerY - Scale(12, scale), centerX + cardWidth * 0.45f, centerY + Scale(34, scale)), primaryText,
                DWRITE_TEXT_ALIGNMENT_CENTER);
            DrawText(L"or choose Open to browse", bodyFormat.Get(), D2D1::RectF(centerX - cardWidth * 0.45f,
                centerY + Scale(40, scale), centerX + cardWidth * 0.45f, centerY + Scale(68, scale)), secondaryText,
                DWRITE_TEXT_ALIGNMENT_CENTER);
            DrawText(L"GLB 2.0  •  embedded geometry", smallFormat.Get(), D2D1::RectF(centerX - cardWidth * 0.45f,
                centerY + Scale(74, scale), centerX + cardWidth * 0.45f, centerY + Scale(100, scale)),
                D2D1::ColorF(0x747B86), DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        if (overlay.state == ViewerState::Failed)
        {
            SetBrush(D2D1::ColorF(0x0B0C0F, 0.40f));
            overlayTarget->FillRectangle(D2D1::RectF(0, toolbar, clientWidth, clientHeight), brush.Get());
            const RECT cardPixels = CalculateErrorCardRect(width, height, overlay.toolbarHeight, scale);
            const D2D1_ROUNDED_RECT card = D2D1::RoundedRect(D2D1::RectF(static_cast<float>(cardPixels.left),
                static_cast<float>(cardPixels.top), static_cast<float>(cardPixels.right), static_cast<float>(cardPixels.bottom)),
                Scale(12, scale), Scale(12, scale));
            SetBrush(D2D1::ColorF(0x242426));
            overlayTarget->FillRoundedRectangle(card, brush.Get());
            SetBrush(D2D1::ColorF(0x3A3A3C));
            overlayTarget->DrawRoundedRectangle(card, brush.Get(), 1.0f);

            const float left = static_cast<float>(cardPixels.left) + Scale(28, scale);
            const float right = static_cast<float>(cardPixels.right) - Scale(28, scale);
            const float top = static_cast<float>(cardPixels.top) + Scale(24, scale);
            SetBrush(D2D1::ColorF(0xFF453A));
            overlayTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(left + Scale(12, scale), top + Scale(12, scale)),
                Scale(12, scale), Scale(12, scale)), brush.Get());
            DrawText(L"!", filenameFormat.Get(), D2D1::RectF(left, top + Scale(1, scale), left + Scale(24, scale),
                top + Scale(24, scale)), D2D1::ColorF(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER);
            DrawText(overlay.errorSummary, filenameFormat.Get(), D2D1::RectF(left + Scale(38, scale), top - Scale(2, scale),
                right, top + Scale(34, scale)), primaryText);
            DrawText(overlay.errorDetails, bodyFormat.Get(), D2D1::RectF(left, top + Scale(52, scale), right,
                static_cast<float>(cardPixels.bottom) - Scale(70, scale)), secondaryText);
            DrawText(L"GLB  •  opening", smallFormat.Get(), D2D1::RectF(left, static_cast<float>(cardPixels.bottom) - Scale(101, scale),
                right, static_cast<float>(cardPixels.bottom) - Scale(76, scale)), D2D1::ColorF(0x777F8B));
        }

        if (!overlay.warning.empty() && overlay.state == ViewerState::Ready)
        {
            const float size = Scale(30, scale);
            const float right = contentRight - Scale(14, scale);
            const float bottom = contentBottom - Scale(14, scale);
            SetBrush(D2D1::ColorF(0xFF9F0A, 0.92f));
            overlayTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(right - size * 0.5f, bottom - size * 0.5f),
                size * 0.5f, size * 0.5f), brush.Get());
            DrawText(L"!", filenameFormat.Get(), D2D1::RectF(right - size, bottom - size + Scale(2, scale), right, bottom),
                D2D1::ColorF(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        // Transient mode readouts (fly speed, grid/projection toggles).
        auto drawHud = [&](const std::wstring& text, float alpha, float bottomOffset)
        {
            if (alpha <= 0.01f || text.empty() || overlay.state != ViewerState::Ready) return;
            const float pillHeight = Scale(26, scale);
            const float pillWidth = std::min(contentRight - Scale(28, scale),
                std::max(Scale(96, scale), Scale(14, scale) + static_cast<float>(text.size()) * Scale(7.6f, scale)));
            const float left = (contentRight - pillWidth) * 0.5f;
            const float top = contentBottom - bottomOffset - pillHeight;
            const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(D2D1::RectF(left, top, left + pillWidth, top + pillHeight),
                pillHeight * 0.5f, pillHeight * 0.5f);
            SetBrush(D2D1::ColorF(0x111318, 0.84f * alpha));
            overlayTarget->FillRoundedRectangle(pill, brush.Get());
            DrawText(text, smallFormat.Get(), D2D1::RectF(left, top, left + pillWidth, top + pillHeight),
                D2D1::ColorF(0xE8EAED, alpha), DWRITE_TEXT_ALIGNMENT_CENTER);
        };
        drawHud(overlay.speedHud, overlay.speedHudAlpha, Scale(84, scale));
        drawHud(overlay.modeHud, overlay.modeHudAlpha, Scale(50, scale));

        DrawBottomBar(overlay, clientWidth, clientHeight, scale);
        DrawInfoPanel(overlay, clientWidth, clientHeight, scale);

        // Navigation gizmo on top of everything else, except a floating
        // flyout (Speed), which floats above even that.
        if (overlay.state == ViewerState::Ready && overlay.hasModel)
        {
            DrawGizmo(camera, gizmo, scale);
        }
        DrawSpeedFlyout(overlay, scale);
        DrawTooltip(overlay, clientWidth, scale);

        if (overlayTarget->EndDraw() == D2DERR_RECREATE_TARGET)
        {
            overlayTarget.Reset();
            brush.Reset();
        }
    }

    bool CreateGrid(const ModelData& model, std::wstring& error)
    {
        std::vector<ModelVertex> vertices;
        const float centerX = (model.boundsMin.x + model.boundsMax.x) * 0.5f;
        const float centerZ = (model.boundsMin.z + model.boundsMax.z) * 0.5f;
        const float extentX = model.boundsMax.x - model.boundsMin.x;
        const float extentY = model.boundsMax.y - model.boundsMin.y;
        const float extentZ = model.boundsMax.z - model.boundsMin.z;
        const float radius = std::max(0.001f, std::sqrt(extentX * extentX + extentY * extentY + extentZ * extentZ) * 0.5f);
        const float span = radius * 2.2f;
        const float ground = model.boundsMin.y - radius * 0.012f;
        constexpr int divisions = 20;
        vertices.reserve((divisions + 1) * 4);
        for (int line = 0; line <= divisions; ++line)
        {
            const float t = static_cast<float>(line) / divisions;
            const float offset = -span + 2.0f * span * t;
            const bool major = line == divisions / 2 || line % 5 == 0;
            const XMFLOAT4 color = major ? XMFLOAT4(0.22f, 0.25f, 0.30f, 0.52f) : XMFLOAT4(0.16f, 0.18f, 0.22f, 0.36f);
            vertices.push_back({ XMFLOAT3(centerX - span, ground, centerZ + offset), XMFLOAT3(0, 1, 0), color });
            vertices.push_back({ XMFLOAT3(centerX + span, ground, centerZ + offset), XMFLOAT3(0, 1, 0), color });
            vertices.push_back({ XMFLOAT3(centerX + offset, ground, centerZ - span), XMFLOAT3(0, 1, 0), color });
            vertices.push_back({ XMFLOAT3(centerX + offset, ground, centerZ + span), XMFLOAT3(0, 1, 0), color });
        }
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(ModelVertex));
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA initial{ vertices.data(), 0, 0 };
        if (FAILED(device->CreateBuffer(&description, &initial, &gridBuffer)))
        {
            error = L"The ground grid could not be uploaded.";
            return false;
        }
        gridVertexCount = static_cast<UINT>(vertices.size());
        return true;
    }
};

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;

bool Renderer::Initialize(HWND window, std::wstring& error)
{
    impl_->window = window;
    RECT client{};
    GetClientRect(window, &client);
    impl_->width = std::max(1L, client.right - client.left);
    impl_->height = std::max(1L, client.bottom - client.top);

    DXGI_SWAP_CHAIN_DESC swapDescription{};
    swapDescription.BufferDesc.Width = impl_->width;
    swapDescription.BufferDesc.Height = impl_->height;
    swapDescription.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapDescription.BufferDesc.RefreshRate.Numerator = 60;
    swapDescription.BufferDesc.RefreshRate.Denominator = 1;
    swapDescription.SampleDesc.Count = 1;
    swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDescription.BufferCount = 2;
    swapDescription.OutputWindow = window;
    swapDescription.Windowed = TRUE;
    swapDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    const D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL createdLevel{};
    HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
        &swapDescription, &impl_->swapChain, &impl_->device, &createdLevel, &impl_->context);
    if (FAILED(result))
    {
        result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
            &swapDescription, &impl_->swapChain, &impl_->device, &createdLevel, &impl_->context);
    }
    if (FAILED(result))
    {
        error = L"A Direct3D 11 device could not be created.";
        return false;
    }

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, impl_->d2dFactory.GetAddressOf())) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(impl_->writeFactory.GetAddressOf()))))
    {
        error = L"The Windows text renderer could not be initialized.";
        return false;
    }

    D2D1_STROKE_STYLE_PROPERTIES strokeProperties = D2D1::StrokeStyleProperties();
    strokeProperties.dashStyle = D2D1_DASH_STYLE_DASH;
    impl_->d2dFactory->CreateStrokeStyle(strokeProperties, nullptr, 0, &impl_->dashedStroke);
    D2D1_STROKE_STYLE_PROPERTIES spinnerProperties = D2D1::StrokeStyleProperties();
    spinnerProperties.startCap = D2D1_CAP_STYLE_ROUND;
    spinnerProperties.endCap = D2D1_CAP_STYLE_ROUND;
    impl_->d2dFactory->CreateStrokeStyle(spinnerProperties, nullptr, 0, &impl_->spinnerStroke);

    ComPtr<ID3DBlob> vertexCode = CompileShader(kVertexShader, "main", "vs_5_0", error);
    ComPtr<ID3DBlob> pixelCode = CompileShader(kPixelShader, "main", "ps_5_0", error);
    if (!vertexCode || !pixelCode) return false;
    if (FAILED(impl_->device->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr,
        &impl_->vertexShader)) || FAILED(impl_->device->CreatePixelShader(pixelCode->GetBufferPointer(),
            pixelCode->GetBufferSize(), nullptr, &impl_->pixelShader)))
    {
        error = L"The built-in graphics pipeline could not be created.";
        return false;
    }

    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(ModelVertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(ModelVertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(ModelVertex, color), D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    if (FAILED(impl_->device->CreateInputLayout(layout, ARRAYSIZE(layout), vertexCode->GetBufferPointer(),
        vertexCode->GetBufferSize(), &impl_->inputLayout)))
    {
        error = L"The model vertex layout could not be created.";
        return false;
    }

    D3D11_BUFFER_DESC constantDescription{};
    constantDescription.ByteWidth = sizeof(FrameConstants);
    constantDescription.Usage = D3D11_USAGE_DEFAULT;
    constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(impl_->device->CreateBuffer(&constantDescription, nullptr, &impl_->constantBuffer)))
    {
        error = L"The camera buffer could not be created.";
        return false;
    }

    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    impl_->device->CreateRasterizerState(&rasterizer, &impl_->rasterizerState);

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    impl_->device->CreateDepthStencilState(&depth, &impl_->depthState);

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    impl_->device->CreateBlendState(&blend, &impl_->blendState);

    return impl_->CreateTargets(error);
}

bool Renderer::Resize(int width, int height, std::wstring& error)
{
    if (!impl_->swapChain || width <= 0 || height <= 0) return true;
    if (width == impl_->width && height == impl_->height && impl_->renderTarget) return true;
    impl_->context->OMSetRenderTargets(0, nullptr, nullptr);
    impl_->overlayTarget.Reset();
    impl_->brush.Reset();
    impl_->renderTarget.Reset();
    impl_->depthView.Reset();
    impl_->depthTexture.Reset();
    impl_->context->Flush();
    const HRESULT result = impl_->swapChain->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height),
        DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(result))
    {
        error = L"The 3D viewport could not be resized.";
        return false;
    }
    impl_->width = width;
    impl_->height = height;
    return impl_->CreateTargets(error);
}

bool Renderer::UploadModel(const ModelData& model, std::wstring& error)
{
    if (!impl_->device || model.vertices.empty() || model.indices.empty())
    {
        error = L"The imported model has no renderable triangle data.";
        return false;
    }
    if (model.vertices.size() * sizeof(ModelVertex) > std::numeric_limits<UINT>::max() ||
        model.indices.size() * sizeof(std::uint32_t) > std::numeric_limits<UINT>::max())
    {
        error = L"The model is too large for a single preview buffer.";
        return false;
    }

    ComPtr<ID3D11Buffer> vertexBuffer;
    D3D11_BUFFER_DESC vertexDescription{};
    vertexDescription.ByteWidth = static_cast<UINT>(model.vertices.size() * sizeof(ModelVertex));
    vertexDescription.Usage = D3D11_USAGE_IMMUTABLE;
    vertexDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vertexData{ model.vertices.data(), 0, 0 };
    if (FAILED(impl_->device->CreateBuffer(&vertexDescription, &vertexData, &vertexBuffer)))
    {
        error = L"There was not enough graphics memory to upload this model.";
        return false;
    }

    ComPtr<ID3D11Buffer> indexBuffer;
    D3D11_BUFFER_DESC indexDescription{};
    indexDescription.ByteWidth = static_cast<UINT>(model.indices.size() * sizeof(std::uint32_t));
    indexDescription.Usage = D3D11_USAGE_IMMUTABLE;
    indexDescription.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA indexData{ model.indices.data(), 0, 0 };
    if (FAILED(impl_->device->CreateBuffer(&indexDescription, &indexData, &indexBuffer)))
    {
        error = L"There was not enough graphics memory to upload this model.";
        return false;
    }

    impl_->vertexBuffer = std::move(vertexBuffer);
    impl_->indexBuffer = std::move(indexBuffer);
    impl_->indexCount = static_cast<UINT>(model.indices.size());
    impl_->boundsMin = model.boundsMin;
    impl_->boundsMax = model.boundsMax;
    impl_->gridBuffer.Reset();
    impl_->gridVertexCount = 0;
    return impl_->CreateGrid(model, error);
}

void Renderer::ClearModel()
{
    impl_->vertexBuffer.Reset();
    impl_->indexBuffer.Reset();
    impl_->gridBuffer.Reset();
    impl_->indexCount = 0;
    impl_->gridVertexCount = 0;
}

bool Renderer::HasModel() const
{
    return impl_->vertexBuffer && impl_->indexBuffer && impl_->indexCount > 0;
}

void Renderer::Render(const Camera& camera, const OverlayInfo& overlay, const NavGizmo& gizmo, const Chrome& chrome)
{
    if (!impl_->context || !impl_->renderTarget || impl_->width <= 0 || impl_->height <= 0) return;
    ID3D11RenderTargetView* renderTarget = impl_->renderTarget.Get();
    impl_->context->OMSetRenderTargets(1, &renderTarget, impl_->depthView.Get());
    const float background[] = { 28.0f / 255.0f, 28.0f / 255.0f, 30.0f / 255.0f, 1.0f };
    impl_->context->ClearRenderTargetView(impl_->renderTarget.Get(), background);
    impl_->context->ClearDepthStencilView(impl_->depthView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    const bool loading = overlay.state == ViewerState::Loading;
    const float viewportTop = loading ? 0.0f : static_cast<float>(overlay.toolbarHeight);
    const float bottomInset = loading ? 0.0f : static_cast<float>(overlay.bottomBarHeight);
    const float panelInset = loading ? 0.0f : static_cast<float>(overlay.infoPanelWidth);
    const float viewportWidth = std::max(1.0f, static_cast<float>(impl_->width) - panelInset);
    const float viewportHeight = std::max(1.0f, static_cast<float>(impl_->height) - viewportTop - bottomInset);
    D3D11_VIEWPORT viewport{ 0.0f, viewportTop, viewportWidth, viewportHeight, 0.0f, 1.0f };
    impl_->context->RSSetViewports(1, &viewport);
    impl_->context->RSSetState(impl_->rasterizerState.Get());
    impl_->context->OMSetDepthStencilState(impl_->depthState.Get(), 0);
    const float blendFactor[] = { 0, 0, 0, 0 };
    impl_->context->OMSetBlendState(impl_->blendState.Get(), blendFactor, 0xffffffff);
    impl_->context->IASetInputLayout(impl_->inputLayout.Get());
    impl_->context->VSSetShader(impl_->vertexShader.Get(), nullptr, 0);
    impl_->context->PSSetShader(impl_->pixelShader.Get(), nullptr, 0);

    const XMVECTOR eye = camera.EyePosition();
    const XMMATRIX view = camera.ViewMatrix();
    const float aspect = viewportWidth / viewportHeight;
    const XMMATRIX projection = camera.ProjectionMatrix(aspect);
    FrameConstants constants;
    XMStoreFloat4x4(&constants.viewProjection, view * projection);
    XMStoreFloat4(&constants.cameraPosition, eye);
    constants.options = XMFLOAT4(overlay.selectionAmount, 0.0f, 0.0f, 0.0f);
    impl_->context->UpdateSubresource(impl_->constantBuffer.Get(), 0, nullptr, &constants, 0, 0);
    ID3D11Buffer* constantBuffer = impl_->constantBuffer.Get();
    impl_->context->VSSetConstantBuffers(0, 1, &constantBuffer);
    impl_->context->PSSetConstantBuffers(0, 1, &constantBuffer);

    const UINT stride = sizeof(ModelVertex);
    const UINT offsetBytes = 0;
    if (!loading && impl_->gridBuffer && overlay.gridVisible)
    {
        ID3D11Buffer* grid = impl_->gridBuffer.Get();
        impl_->context->IASetVertexBuffers(0, 1, &grid, &stride, &offsetBytes);
        impl_->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        impl_->context->Draw(impl_->gridVertexCount, 0);
    }
    if (!loading && impl_->vertexBuffer && impl_->indexBuffer)
    {
        ID3D11Buffer* vertices = impl_->vertexBuffer.Get();
        impl_->context->IASetVertexBuffers(0, 1, &vertices, &stride, &offsetBytes);
        impl_->context->IASetIndexBuffer(impl_->indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
        impl_->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        impl_->context->DrawIndexed(impl_->indexCount, 0, 0);
    }

    impl_->DrawOverlay(camera, overlay, gizmo, chrome);
    impl_->swapChain->Present(1, 0);
}
