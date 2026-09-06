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
constexpr float kVerticalFieldOfView = XM_PIDIV4;

float Scale(float value, float dpiScale)
{
    return std::round(value * dpiScale);
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
    return float4(lit, input.color.a);
}
)";

struct FrameConstants
{
    XMFLOAT4X4 viewProjection{};
    XMFLOAT4 cameraPosition{};
};
}

void Camera::SetBounds(const XMFLOAT3& minimum, const XMFLOAT3& maximum, float aspect)
{
    targetX = (static_cast<double>(minimum.x) + maximum.x) * 0.5;
    targetY = (static_cast<double>(minimum.y) + maximum.y) * 0.5;
    targetZ = (static_cast<double>(minimum.z) + maximum.z) * 0.5;
    const double extentX = static_cast<double>(maximum.x) - minimum.x;
    const double extentY = static_cast<double>(maximum.y) - minimum.y;
    const double extentZ = static_cast<double>(maximum.z) - minimum.z;
    sceneRadius = std::max(0.0001, std::sqrt(extentX * extentX + extentY * extentY + extentZ * extentZ) * 0.5);
    Reset(aspect);
}

void Camera::Fit(float aspect)
{
    const double vertical = kVerticalFieldOfView;
    const double horizontal = 2.0 * std::atan(std::tan(vertical * 0.5) * std::max(0.1f, aspect));
    const double limitingFov = std::min(vertical, horizontal);
    distance = sceneRadius / std::max(0.05, std::sin(limitingFov * 0.5));
    distance /= 0.93;
}

void Camera::Reset(float aspect)
{
    const XMVECTOR value = XMQuaternionRotationRollPitchYaw(-0.30f, 0.58f, 0.0f);
    XMStoreFloat4(&orientation, value);
    Fit(aspect);
}

void Camera::Orbit(float deltaX, float deltaY)
{
    XMVECTOR current = LoadOrientation(*this);
    const XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
    const XMVECTOR right = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), current);
    const XMVECTOR yaw = XMQuaternionRotationAxis(worldUp, -deltaX * 0.008f);
    const XMVECTOR pitch = XMQuaternionRotationAxis(right, -deltaY * 0.008f);
    current = XMQuaternionNormalize(XMQuaternionMultiply(XMQuaternionMultiply(current, yaw), pitch));
    XMStoreFloat4(&orientation, current);
}

void Camera::Look(float deltaX, float deltaY)
{
    XMVECTOR current = LoadOrientation(*this);
    const XMVECTOR target = XMVectorSet(static_cast<float>(targetX), static_cast<float>(targetY),
        static_cast<float>(targetZ), 1.0f);
    const XMVECTOR eye = target + XMVector3Rotate(XMVectorSet(0, 0, static_cast<float>(distance), 0), current);

    const XMVECTOR up = XMVector3Rotate(XMVectorSet(0, 1, 0, 0), current);
    const XMVECTOR yaw = XMQuaternionRotationAxis(up, -deltaX * 0.006f);
    current = XMQuaternionNormalize(XMQuaternionMultiply(current, yaw));
    const XMVECTOR right = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), current);
    const XMVECTOR pitch = XMQuaternionRotationAxis(right, -deltaY * 0.006f);
    current = XMQuaternionNormalize(XMQuaternionMultiply(current, pitch));

    const XMVECTOR newTarget = eye - XMVector3Rotate(
        XMVectorSet(0, 0, static_cast<float>(distance), 0), current);
    XMFLOAT3 targetValue{};
    XMStoreFloat3(&targetValue, newTarget);
    targetX = targetValue.x;
    targetY = targetValue.y;
    targetZ = targetValue.z;
    XMStoreFloat4(&orientation, current);
}

void Camera::Pan(float deltaX, float deltaY, float viewportHeight)
{
    if (viewportHeight <= 1.0f) return;
    const XMVECTOR current = LoadOrientation(*this);
    XMFLOAT3 right{};
    XMFLOAT3 up{};
    XMStoreFloat3(&right, XMVector3Rotate(XMVectorSet(1, 0, 0, 0), current));
    XMStoreFloat3(&up, XMVector3Rotate(XMVectorSet(0, 1, 0, 0), current));
    const double unitsPerPixel = 2.0 * distance * std::tan(kVerticalFieldOfView * 0.5) / viewportHeight;
    targetX += (-right.x * deltaX + up.x * deltaY) * unitsPerPixel;
    targetY += (-right.y * deltaX + up.y * deltaY) * unitsPerPixel;
    targetZ += (-right.z * deltaX + up.z * deltaY) * unitsPerPixel;
}

void Camera::Dolly(float wheelSteps)
{
    distance *= std::exp(-static_cast<double>(wheelSteps) * 0.16);
    distance = std::clamp(distance, sceneRadius * 0.025, sceneRadius * 250.0);
}

void Camera::MoveLocal(float rightAmount, float upAmount, float forwardAmount)
{
    const XMVECTOR current = LoadOrientation(*this);
    const XMVECTOR right = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), current);
    const XMVECTOR up = XMVector3Rotate(XMVectorSet(0, 1, 0, 0), current);
    const XMVECTOR forward = XMVector3Rotate(XMVectorSet(0, 0, -1, 0), current);
    XMFLOAT3 movement{};
    XMStoreFloat3(&movement, right * rightAmount + up * upAmount + forward * forwardAmount);
    targetX += movement.x;
    targetY += movement.y;
    targetZ += movement.z;
}

void Camera::Roll(float radians)
{
    XMVECTOR current = LoadOrientation(*this);
    const XMVECTOR forward = XMVector3Rotate(XMVectorSet(0, 0, -1, 0), current);
    const XMVECTOR roll = XMQuaternionRotationAxis(forward, radians);
    current = XMQuaternionNormalize(XMQuaternionMultiply(current, roll));
    XMStoreFloat4(&orientation, current);
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
        textScale = scale;
        auto create = [&](float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& output)
        {
            return SUCCEEDED(writeFactory->CreateTextFormat(L"Segoe UI Variable Text", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, Scale(size, scale), L"en-us", &output));
        };
        if (!create(22, DWRITE_FONT_WEIGHT_SEMI_BOLD, headingFormat) ||
            !create(14, DWRITE_FONT_WEIGHT_NORMAL, bodyFormat) ||
            !create(12, DWRITE_FONT_WEIGHT_NORMAL, smallFormat) ||
            !create(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, filenameFormat)) return false;
        headingFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        headingFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        filenameFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        smallFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
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

    void DrawOverlay(const OverlayInfo& overlay)
    {
        if (!overlayTarget || !brush) return;
        if (overlay.state != ViewerState::Loading && !CreateTextFormats(overlay.dpiScale)) return;
        const float scale = overlay.dpiScale;
        const float toolbar = static_cast<float>(overlay.toolbarHeight);
        const float clientWidth = static_cast<float>(width);
        const float clientHeight = static_cast<float>(height);

        overlayTarget->BeginDraw();
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

        SetBrush(D2D1::ColorF(0x2C2C2E));
        overlayTarget->FillRectangle(D2D1::RectF(0, 0, clientWidth, toolbar), brush.Get());
        SetBrush(D2D1::ColorF(0x3A3A3C));
        overlayTarget->FillRectangle(D2D1::RectF(0, toolbar - 1, clientWidth, toolbar), brush.Get());

        const float badgeLeft = Scale(12, scale);
        const float badgeTop = Scale(12, scale);
        const float badgeSize = Scale(28, scale);
        SetBrush(D2D1::ColorF(0x3A3A3C));
        overlayTarget->FillRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(badgeLeft, badgeTop, badgeLeft + badgeSize, badgeTop + badgeSize), Scale(7, scale), Scale(7, scale)), brush.Get());
        DrawText(L"3D", smallFormat.Get(), D2D1::RectF(badgeLeft, badgeTop + Scale(4, scale), badgeLeft + badgeSize,
            badgeTop + badgeSize), D2D1::ColorF(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER);

        const std::wstring filename = overlay.filename.empty() ? L"3D Preview" : overlay.filename;
        const float titleWidth = std::min(Scale(360, scale), std::max(120.0f, clientWidth - Scale(620, scale)));
        DrawText(filename, filenameFormat.Get(), D2D1::RectF((clientWidth - titleWidth) * 0.5f, 0,
            (clientWidth + titleWidth) * 0.5f, toolbar), D2D1::ColorF(0xF5F5F7), DWRITE_TEXT_ALIGNMENT_CENTER);

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

        if (!overlay.status.empty() && (overlay.state == ViewerState::Ready || (overlay.state == ViewerState::Loading && overlay.hasModel)))
        {
            const float margin = Scale(14, scale);
            const float pillHeight = Scale(30, scale);
            const float pillWidth = std::min(clientWidth - margin * 2, Scale(350, scale));
            const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(D2D1::RectF(margin, clientHeight - margin - pillHeight,
                margin + pillWidth, clientHeight - margin), pillHeight * 0.5f, pillHeight * 0.5f);
            SetBrush(D2D1::ColorF(0x111318, 0.82f));
            overlayTarget->FillRoundedRectangle(pill, brush.Get());
            DrawText(overlay.status, smallFormat.Get(), D2D1::RectF(margin + Scale(12, scale), clientHeight - margin - pillHeight,
                margin + pillWidth - Scale(10, scale), clientHeight - margin - Scale(1, scale)), secondaryText);
        }

        if (!overlay.warning.empty() && overlay.state == ViewerState::Ready)
        {
            const float size = Scale(30, scale);
            const float right = clientWidth - Scale(14, scale);
            const float bottom = clientHeight - Scale(14, scale);
            SetBrush(D2D1::ColorF(0xFF9F0A, 0.92f));
            overlayTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(right - size * 0.5f, bottom - size * 0.5f),
                size * 0.5f, size * 0.5f), brush.Get());
            DrawText(L"!", filenameFormat.Get(), D2D1::RectF(right - size, bottom - size + Scale(2, scale), right, bottom),
                D2D1::ColorF(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER);
        }

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

void Renderer::Render(const Camera& camera, const OverlayInfo& overlay)
{
    if (!impl_->context || !impl_->renderTarget || impl_->width <= 0 || impl_->height <= 0) return;
    ID3D11RenderTargetView* renderTarget = impl_->renderTarget.Get();
    impl_->context->OMSetRenderTargets(1, &renderTarget, impl_->depthView.Get());
    const float background[] = { 28.0f / 255.0f, 28.0f / 255.0f, 30.0f / 255.0f, 1.0f };
    impl_->context->ClearRenderTargetView(impl_->renderTarget.Get(), background);
    impl_->context->ClearDepthStencilView(impl_->depthView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    const bool loading = overlay.state == ViewerState::Loading;
    const float viewportTop = loading ? 0.0f : static_cast<float>(overlay.toolbarHeight);
    const float viewportHeight = std::max(1.0f, static_cast<float>(impl_->height) - viewportTop);
    D3D11_VIEWPORT viewport{ 0.0f, viewportTop, static_cast<float>(impl_->width), viewportHeight, 0.0f, 1.0f };
    impl_->context->RSSetViewports(1, &viewport);
    impl_->context->RSSetState(impl_->rasterizerState.Get());
    impl_->context->OMSetDepthStencilState(impl_->depthState.Get(), 0);
    const float blendFactor[] = { 0, 0, 0, 0 };
    impl_->context->OMSetBlendState(impl_->blendState.Get(), blendFactor, 0xffffffff);
    impl_->context->IASetInputLayout(impl_->inputLayout.Get());
    impl_->context->VSSetShader(impl_->vertexShader.Get(), nullptr, 0);
    impl_->context->PSSetShader(impl_->pixelShader.Get(), nullptr, 0);

    const XMVECTOR orientation = LoadOrientation(camera);
    const XMVECTOR offset = XMVector3Rotate(XMVectorSet(0, 0, static_cast<float>(camera.distance), 0), orientation);
    const XMVECTOR target = XMVectorSet(static_cast<float>(camera.targetX), static_cast<float>(camera.targetY),
        static_cast<float>(camera.targetZ), 1);
    const XMVECTOR eye = target + offset;
    XMVECTOR up = XMVector3Rotate(XMVectorSet(0, 1, 0, 0), orientation);
    const XMMATRIX view = XMMatrixLookAtRH(eye, target, up);
    const float aspect = static_cast<float>(impl_->width) / viewportHeight;
    const float nearPlane = static_cast<float>(std::max(camera.sceneRadius * 0.00001, camera.distance * 0.0005));
    const XMVECTOR sceneCenter = XMVectorSet(
        (impl_->boundsMin.x + impl_->boundsMax.x) * 0.5f,
        (impl_->boundsMin.y + impl_->boundsMax.y) * 0.5f,
        (impl_->boundsMin.z + impl_->boundsMax.z) * 0.5f, 1.0f);
    const double distanceToScene = static_cast<double>(XMVectorGetX(XMVector3Length(sceneCenter - eye)));
    const float farPlane = static_cast<float>(std::max(
        distanceToScene + camera.sceneRadius * 8.0, static_cast<double>(nearPlane) * 100.0));
    const XMMATRIX projection = XMMatrixPerspectiveFovRH(kVerticalFieldOfView, std::max(0.05f, aspect), nearPlane, farPlane);
    FrameConstants constants;
    XMStoreFloat4x4(&constants.viewProjection, view * projection);
    XMStoreFloat4(&constants.cameraPosition, eye);
    impl_->context->UpdateSubresource(impl_->constantBuffer.Get(), 0, nullptr, &constants, 0, 0);
    ID3D11Buffer* constantBuffer = impl_->constantBuffer.Get();
    impl_->context->VSSetConstantBuffers(0, 1, &constantBuffer);
    impl_->context->PSSetConstantBuffers(0, 1, &constantBuffer);

    const UINT stride = sizeof(ModelVertex);
    const UINT offsetBytes = 0;
    if (!loading && impl_->gridBuffer)
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

    impl_->DrawOverlay(overlay);
    impl_->swapChain->Present(1, 0);
}
