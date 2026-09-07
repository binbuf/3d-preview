#pragma once

#include "Model.h"

#include <cstdint>
#include <string>
#include <vector>

// Read-only "Stats & Shading" side panel content, adapted from the fields the
// (now defunct) Windows 11 3D Viewer showed. Pure data/layout — no D2D or
// window dependency, the same split as NavGizmo vs. Renderer::DrawGizmo:
// Renderer::DrawInfoPanel does the actual text drawing row-by-row within the
// rect ComputeInfoPanelLayout returns.
struct InfoPanelRow
{
    std::wstring label;
    std::wstring value;
};

struct InfoPanelSection
{
    std::wstring title;
    std::vector<InfoPanelRow> rows;
};

// Builds the section/row text for a loaded model's scanned stats (see
// ModelStats, Model.h). Fields the current GLB-only slice cannot populate
// (texture/animation data on a model that has none) still get a row, reading
// "0" or "No" rather than being omitted, so the panel's shape is stable.
std::vector<InfoPanelSection> BuildInfoPanelSections(
    const ModelStats& stats, std::uint64_t triangleCount, std::uint64_t vertexCount);

// Fixed-width panel docked to the right edge of the viewport, below the title
// bar and above the bottom bar.
struct InfoPanelLayout
{
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    float Width() const { return right - left; }
};

InfoPanelLayout ComputeInfoPanelLayout(int viewportWidth, int viewportHeight,
    int titleBarHeight, int bottomBarHeight, float dpiScale);
