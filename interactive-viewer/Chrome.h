#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Windows-11-Photos-style unified title bar: pure layout/hit-test state, no
// drawing or COM dependency — mirrors NavGizmo's split from Renderer::DrawGizmo.
// Renderer::DrawTitleBar consumes this class's rects to paint the D2D content;
// Preview3D.cpp's WM_NCHITTEST/WM_LBUTTONDOWN handlers consume HitTest to route
// input. All rects are in client pixel coordinates.
class Chrome
{
public:
    enum class Part
    {
        None,
        Caption,       // empty drag strip
        SystemIcon,    // top-left icon: opens the system menu
        Grid,
        AxisSnap,
        Speed,
        Fit,
        Reset,
        Info,
        Share,
        Overflow,      // "..." menu (Controls/warnings/About)
        OpenWith,
        Minimize,
        Maximize,
        Close
    };

    struct ButtonState
    {
        RECT rect{};
        bool enabled = true;
        bool visible = true;
    };

    // Recomputes every button rect for the current client width and DPI.
    // `hasModel` hides the navigation-only buttons (Grid/Snap/Speed/Fit/Reset)
    // the same way the old toolbar disabled them with nothing loaded.
    void UpdateLayout(int clientWidth, int titleBarHeight, float dpiScale, bool hasModel, bool isMaximized);

    // Hit-tests a CLIENT-coordinate point (already converted from screen
    // coordinates by the caller) against every button, falling back to
    // Part::Caption inside the bar's empty drag area and Part::None outside it
    // entirely.
    Part HitTest(POINT clientPoint) const;

    const ButtonState& Button(Part part) const;
    RECT TitleBarRect() const { return titleBarRect_; }
    RECT FilenameRect() const { return filenameRect_; }
    bool Maximized() const { return isMaximized_; }

    Part hover = Part::None;
    Part pressed = Part::None;

private:
    RECT titleBarRect_{};
    RECT filenameRect_{};
    bool isMaximized_ = false;
    ButtonState systemIcon_;
    ButtonState grid_;
    ButtonState axisSnap_;
    ButtonState speed_;
    ButtonState fit_;
    ButtonState reset_;
    ButtonState info_;
    ButtonState share_;
    ButtonState overflow_;
    ButtonState openWith_;
    ButtonState minimize_;
    ButtonState maximize_;
    ButtonState close_;
};
