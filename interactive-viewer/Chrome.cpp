#include "Chrome.h"

#include <algorithm>
#include <cmath>

namespace
{
int Scale(int logical, float dpiScale)
{
    return static_cast<int>(std::lround(logical * std::max(0.75f, dpiScale)));
}

RECT MakeRect(int left, int top, int width, int height)
{
    return RECT{ left, top, left + width, top + height };
}

bool PtInRectValue(const RECT& rect, POINT point)
{
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
}
}

void Chrome::UpdateLayout(int clientWidth, int titleBarHeight, float dpiScale, bool hasModel, bool isMaximized)
{
    titleBarRect_ = RECT{ 0, 0, clientWidth, titleBarHeight };
    isMaximized_ = isMaximized;

    const int margin = Scale(10, dpiScale);
    const int gap = Scale(4, dpiScale);
    const int sysButtonWidth = Scale(46, dpiScale);
    const int actionGap = Scale(6, dpiScale);
    const int actionHeight = Scale(30, dpiScale);
    const int actionY = (titleBarHeight - actionHeight) / 2;

    // System caption buttons: rightmost, full title-bar height (matches the
    // real caption buttons' hit target so DWM's Snap Layout hover flyout on
    // Maximize feels native). `isMaximized` only affects the drawn glyph
    // (Renderer::DrawTitleBar), not this layout.
    int right = clientWidth;
    close_.rect = MakeRect(right - sysButtonWidth, 0, sysButtonWidth, titleBarHeight); right -= sysButtonWidth;
    maximize_.rect = MakeRect(right - sysButtonWidth, 0, sysButtonWidth, titleBarHeight); right -= sysButtonWidth;
    minimize_.rect = MakeRect(right - sysButtonWidth, 0, sysButtonWidth, titleBarHeight); right -= sysButtonWidth;
    close_.enabled = close_.visible = true;
    maximize_.enabled = maximize_.visible = true;
    minimize_.enabled = minimize_.visible = true;

    right -= margin;

    const int openWithWidth = Scale(96, dpiScale);
    openWith_.rect = MakeRect(right - openWithWidth, actionY, openWithWidth, actionHeight); right -= openWithWidth + actionGap;

    const int overflowWidth = Scale(38, dpiScale);
    overflow_.rect = MakeRect(right - overflowWidth, actionY, overflowWidth, actionHeight); right -= overflowWidth + gap;
    overflow_.enabled = overflow_.visible = true;
    openWith_.enabled = openWith_.visible = hasModel;

    struct Entry { ButtonState* state; int width; };
    const int shareWidth = Scale(56, dpiScale);
    const int infoWidth = Scale(48, dpiScale);
    const int resetWidth = Scale(58, dpiScale);
    const int fitWidth = Scale(44, dpiScale);
    const int speedWidth = Scale(52, dpiScale);
    const int snapWidth = Scale(52, dpiScale);
    const int gridWidth = Scale(48, dpiScale);
    const Entry entries[] = {
        { &share_, shareWidth }, { &info_, infoWidth }, { &reset_, resetWidth }, { &fit_, fitWidth },
        { &speed_, speedWidth }, { &axisSnap_, snapWidth }, { &grid_, gridWidth },
    };
    for (const Entry& entry : entries)
    {
        entry.state->rect = MakeRect(right - entry.width, actionY, entry.width, actionHeight);
        right -= entry.width + actionGap;
        entry.state->enabled = entry.state->visible = hasModel;
    }

    const int rightGroupStart = right - margin;

    const int iconSize = Scale(28, dpiScale);
    systemIcon_.rect = MakeRect(margin, (titleBarHeight - iconSize) / 2, iconSize, iconSize);
    systemIcon_.enabled = systemIcon_.visible = true;

    const int filenameLeft = margin + iconSize + margin;
    filenameRect_ = RECT{ std::min(filenameLeft, rightGroupStart), 0, std::max(filenameLeft, rightGroupStart), titleBarHeight };
}

Chrome::Part Chrome::HitTest(POINT clientPoint) const
{
    if (!PtInRectValue(titleBarRect_, clientPoint)) return Part::None;

    const struct { Part part; const ButtonState* state; } candidates[] = {
        { Part::Close, &close_ }, { Part::Maximize, &maximize_ }, { Part::Minimize, &minimize_ },
        { Part::OpenWith, &openWith_ }, { Part::Overflow, &overflow_ }, { Part::Share, &share_ },
        { Part::Info, &info_ }, { Part::Reset, &reset_ }, { Part::Fit, &fit_ }, { Part::Speed, &speed_ },
        { Part::AxisSnap, &axisSnap_ }, { Part::Grid, &grid_ }, { Part::SystemIcon, &systemIcon_ },
    };
    for (const auto& candidate : candidates)
    {
        if (candidate.state->visible && candidate.state->enabled && PtInRectValue(candidate.state->rect, clientPoint))
        {
            return candidate.part;
        }
    }
    return Part::Caption;
}

const Chrome::ButtonState& Chrome::Button(Part part) const
{
    switch (part)
    {
    case Part::SystemIcon: return systemIcon_;
    case Part::Grid: return grid_;
    case Part::AxisSnap: return axisSnap_;
    case Part::Speed: return speed_;
    case Part::Fit: return fit_;
    case Part::Reset: return reset_;
    case Part::Info: return info_;
    case Part::Share: return share_;
    case Part::Overflow: return overflow_;
    case Part::OpenWith: return openWith_;
    case Part::Minimize: return minimize_;
    case Part::Maximize: return maximize_;
    case Part::Close: return close_;
    default: return systemIcon_;
    }
}
