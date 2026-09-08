#pragma once

#include <DirectXMath.h>

// Canonical axis-aligned view orientations for a Y-up, right-handed world
// (the glTF convention this viewer renders in). "Front" looks along +Z at the
// model, matching the glTF asset front; the reverse views look from behind.
enum class ViewDir
{
    Front,
    Back,
    Left,
    Right,
    Top,
    Bottom
};

// Builds the camera orientation quaternion that looks along `forward` with
// `up` pointing up the screen. The rotation rows are (right, up, back), which
// matches this viewer's camera basis: eye = pivot + rotate((0, 0, distance), q),
// so the returned quaternion drops straight into Camera::orientation.
// A forward parallel to `up` (the pole case) is resolved with a fallback basis
// so the result is never degenerate.
DirectX::XMVECTOR OrientationFromForwardUp(DirectX::XMVECTOR forward, DirectX::XMVECTOR up);
DirectX::XMVECTOR CanonicalViewOrientation(ViewDir view);

// Blender-style navigation gizmo drawn in the top-right corner of the 3D
// viewport. This is pure layout/geometry state with no drawing or window
// dependencies: the app uses HitTest to route pointer input, and the renderer
// projects ComputeDraw output through Direct2D on top of the frame.
class NavGizmo
{
public:
    enum class Part
    {
        None,
        Ball,
        PosX,
        PosY,
        PosZ,
        NegX,
        NegY,
        NegZ
    };

    struct NodeGeometry
    {
        float x = 0.0f;      // screen-space offset from the gizmo center, pixels
        float y = 0.0f;
        float depth = 0.0f;  // view-space z of the node, pixels; > 0 is toward the viewer
    };

    struct DrawGeometry
    {
        float centerX = 0.0f;
        float centerY = 0.0f;
        float outerRadius = 0.0f;
        float nodeRadius = 0.0f;
        float dotRadius = 0.0f;
        float stemWidth = 0.0f;
        NodeGeometry positive[3]{};  // +X, +Y, +Z tips
        NodeGeometry negative[3]{};  // -X, -Y, -Z tails
        Part hover = Part::None;
    };

    // Repositions and resizes the gizmo for the current viewport. Coordinates
    // are client pixels; the top-right placement sits below the title bar and
    // clear of the bottom bar. `viewportWidth` is already narrowed by the
    // caller to exclude the Information panel when it's open, so the gizmo
    // never sits underneath it.
    void UpdateLayout(int viewportWidth, int viewportHeight, int topInset, int bottomInset, float dpiScale);

    // 2.5D raycast hit-test. The pointer spawns a view-space ray against the
    // six axis-node spheres and the center ball; the intersection nearest to
    // the viewer wins, which reproduces the occlusion the painter's-algorithm
    // drawing shows. Axis stems are tested as 2D segments as a fallback.
    Part HitTest(DirectX::XMVECTOR cameraOrientation, float pointerX, float pointerY) const;

    // Projects the world axes through the inverse camera rotation into
    // gizmo-local screen space for this frame's drawing.
    DrawGeometry ComputeDraw(DirectX::XMVECTOR cameraOrientation) const;

    // The orthographic view the camera should animate to when `part` is used.
    ViewDir ViewFor(Part part) const;

    Part hover = Part::None;

private:
    float centerX_ = 0.0f;
    float centerY_ = 0.0f;
    float outer_ = 0.0f;
    float stemLength_ = 0.0f;
    float node_ = 0.0f;
    float dot_ = 0.0f;
    float stemWidth_ = 0.0f;
    float hitSlop_ = 0.0f;
};
