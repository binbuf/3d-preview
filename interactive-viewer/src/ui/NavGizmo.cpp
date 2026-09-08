#include "NavGizmo.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace DirectX;

namespace
{
// Logical (96-DPI) geometry in pixels. The ball is the whole clickable disc;
// stems and nodes are sized so their tips stay inside it.
constexpr float kOuterLogical = 44.0f;
constexpr float kStemLogical = 30.0f;
constexpr float kNodeLogical = 11.5f;
constexpr float kDotLogical = 5.5f;
constexpr float kStemWidthLogical = 2.4f;
constexpr float kCornerMarginLogical = 14.0f;
constexpr float kHitSlopLogical = 3.0f;

void AxisDirections(XMVECTOR cameraOrientation, XMVECTOR (&axes)[3])
{
    // World-to-view rotation: the camera orientation maps camera space into
    // world space, so the conjugate maps the world axes into view space.
    const XMVECTOR inverse = XMQuaternionConjugate(XMQuaternionNormalize(cameraOrientation));
    axes[0] = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), inverse);
    axes[1] = XMVector3Rotate(XMVectorSet(0, 1, 0, 0), inverse);
    axes[2] = XMVector3Rotate(XMVectorSet(0, 0, 1, 0), inverse);
}

int PartIndex(int axis, bool positive)
{
    return static_cast<int>(NavGizmo::Part::PosX) + axis + (positive ? 0 : 3);
}
}

XMVECTOR OrientationFromForwardUp(XMVECTOR forward, XMVECTOR up)
{
    const XMVECTOR back = XMVector3Normalize(XMVectorNegate(forward));
    XMVECTOR right = XMVector3Cross(up, back);
    if (XMVectorGetX(XMVector3LengthSq(right)) < 1e-8f)
    {
        // Pole case: the requested up is parallel to the view axis. Fall back
        // to the next world axis so the basis stays orthonormal.
        const XMVECTOR alternateUp = XMVectorSet(0, 0, 1, 0);
        right = XMVector3Cross(alternateUp, back);
        if (XMVectorGetX(XMVector3LengthSq(right)) < 1e-8f) right = XMVectorSet(1, 0, 0, 0);
        up = alternateUp;
    }
    right = XMVector3Normalize(right);
    const XMVECTOR trueUp = XMVector3Normalize(XMVector3Cross(back, right));
    // XMMATRIX rows are the images of the camera basis vectors under the
    // rotation (DirectXMath row-vector convention): camera +x -> world right,
    // +y -> world up, +z -> world back.
    const XMMATRIX basis(right, trueUp, back, XMVectorSet(0, 0, 0, 1));
    return XMQuaternionNormalize(XMQuaternionRotationMatrix(basis));
}

XMVECTOR CanonicalViewOrientation(ViewDir view)
{
    const XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
    const XMVECTOR worldFront = XMVectorSet(0, 0, -1, 0);
    switch (view)
    {
    case ViewDir::Front: return OrientationFromForwardUp(XMVectorSet(0, 0, 1, 0), worldUp);
    case ViewDir::Back: return OrientationFromForwardUp(worldFront, worldUp);
    case ViewDir::Right: return OrientationFromForwardUp(XMVectorSet(-1, 0, 0, 0), worldUp);
    case ViewDir::Left: return OrientationFromForwardUp(XMVectorSet(1, 0, 0, 0), worldUp);
    case ViewDir::Top: return OrientationFromForwardUp(XMVectorSet(0, -1, 0, 0), XMVectorSet(0, 0, -1, 0));
    case ViewDir::Bottom: return OrientationFromForwardUp(XMVectorSet(0, 1, 0, 0), XMVectorSet(0, 0, -1, 0));
    }
    return XMQuaternionIdentity();
}

void NavGizmo::UpdateLayout(int viewportWidth, int viewportHeight, int topInset, int bottomInset, float dpiScale)
{
    const float scale = std::max(0.75f, dpiScale);
    outer_ = kOuterLogical * scale;
    stemLength_ = kStemLogical * scale;
    node_ = kNodeLogical * scale;
    dot_ = kDotLogical * scale;
    stemWidth_ = kStemWidthLogical * scale;
    hitSlop_ = kHitSlopLogical * scale;
    const float margin = kCornerMarginLogical * scale;
    const float width = std::max(1.0f, static_cast<float>(viewportWidth));
    const float top = static_cast<float>(topInset);
    const float bottom = std::max(top + 1.0f, static_cast<float>(viewportHeight) - static_cast<float>(bottomInset));
    centerX_ = std::max(outer_ + margin, width - margin - outer_);
    // Keep the gizmo inside short viewports rather than overlapping the
    // status area, falling back to the top-bar/bottom-bar edges if space
    // runs out.
    const float highest = top + margin + outer_;
    const float lowest = std::max(top + outer_, bottom - margin - outer_);
    centerY_ = std::min(highest, lowest);
    hover = Part::None;
}

NavGizmo::Part NavGizmo::HitTest(XMVECTOR cameraOrientation, float pointerX, float pointerY) const
{
    const float gx = pointerX - centerX_;
    const float gy = pointerY - centerY_;
    const float reach = outer_ + hitSlop_;
    if (gx * gx + gy * gy > reach * reach) return Part::None;

    XMVECTOR axes[3]{};
    AxisDirections(cameraOrientation, axes);

    // Orthographic view ray: origin (gx, gy, eyeZ), direction (0, 0, -1).
    // Sphere entry t = eyeZ - cz - sqrt(r^2 - dx^2 - dy^2); smallest t wins.
    const float eyeZ = outer_ * 1.5f;
    float bestT = std::numeric_limits<float>::infinity();
    Part best = Part::None;

    for (int axis = 0; axis < 3; ++axis)
    {
        const float ax = XMVectorGetX(axes[axis]);
        const float ay = XMVectorGetY(axes[axis]);
        const float az = XMVectorGetZ(axes[axis]);
        for (int side = 0; side < 2; ++side)
        {
            const float sign = side == 0 ? 1.0f : -1.0f;
            // Screen y grows downward, so the view-space y axis flips.
            const float sx = ax * sign * stemLength_;
            const float sy = -ay * sign * stemLength_;
            const float sz = az * sign * stemLength_;
            const float radius = (side == 0 ? node_ : dot_) + hitSlop_;
            const float dx = gx - sx;
            const float dy = gy - sy;
            const float squared = radius * radius - dx * dx - dy * dy;
            if (squared < 0.0f) continue;
            const float t = eyeZ - sz - std::sqrt(squared);
            if (t < bestT)
            {
                bestT = t;
                best = static_cast<Part>(PartIndex(axis, side == 0));
            }
        }
    }

    // A node/dot hit always wins over the ball: the renderer (DrawGizmo)
    // always paints every node on top of the ball's flat disc regardless of
    // depth, so the ball must never out-rank a node here either. Arbitrating
    // ball-vs-node by the same painter's-algorithm depth as the node-vs-node
    // comparison above (as this used to do) is wrong: the ball is a full
    // sphere of radius `outer_`, which bulges toward the viewer enough at a
    // node's screen offset to beat any node whose axis is closer to
    // edge-on (small view-space depth) — even though that node is clearly
    // visible and unoccluded on screen. That mismatch made clicks on
    // legitimately-visible nodes (e.g. an axis near-perpendicular to the
    // current view) silently grab the ball instead of snapping the view.
    if (best != Part::None) return best;

    // The ball: drag anywhere else on the disc to orbit.
    {
        const float radius = outer_ - 2.0f * hitSlop_;
        const float squared = radius * radius - gx * gx - gy * gy;
        if (squared >= 0.0f) return Part::Ball;
    }

    // Axis stems as 2D segments from the center to each positive tip.
    float bestDistance = std::numeric_limits<float>::infinity();
    Part stem = Part::None;
    for (int axis = 0; axis < 3; ++axis)
    {
        const float tipX = XMVectorGetX(axes[axis]) * stemLength_;
        const float tipY = -XMVectorGetY(axes[axis]) * stemLength_;
        const float lengthSq = tipX * tipX + tipY * tipY;
        const float t = lengthSq > 0.0f
            ? std::clamp((gx * tipX + gy * tipY) / lengthSq, 0.0f, 1.0f)
            : 0.0f;
        const float px = gx - tipX * t;
        const float py = gy - tipY * t;
        const float distance = px * px + py * py;
        if (distance < bestDistance)
        {
            bestDistance = distance;
            stem = static_cast<Part>(PartIndex(axis, true));
        }
    }
    const float stemReach = stemWidth_ * 2.0f + hitSlop_;
    if (stem != Part::None && bestDistance <= stemReach * stemReach) return stem;
    return Part::None;
}

NavGizmo::DrawGeometry NavGizmo::ComputeDraw(XMVECTOR cameraOrientation) const
{
    DrawGeometry geometry;
    geometry.centerX = centerX_;
    geometry.centerY = centerY_;
    geometry.outerRadius = outer_;
    geometry.nodeRadius = node_;
    geometry.dotRadius = dot_;
    geometry.stemWidth = stemWidth_;
    geometry.hover = hover;

    XMVECTOR axes[3]{};
    AxisDirections(cameraOrientation, axes);
    for (int axis = 0; axis < 3; ++axis)
    {
        const float ax = XMVectorGetX(axes[axis]);
        const float ay = XMVectorGetY(axes[axis]);
        const float az = XMVectorGetZ(axes[axis]);
        geometry.positive[axis] = { ax * stemLength_, -ay * stemLength_, az * stemLength_ };
        geometry.negative[axis] = { -ax * stemLength_, ay * stemLength_, -az * stemLength_ };
    }
    return geometry;
}

ViewDir NavGizmo::ViewFor(Part part) const
{
    // Clicking a node snaps the camera to that side of the model: the +X node
    // parks the eye on +X looking down -X (Right), and so on.
    switch (part)
    {
    case Part::PosX: return ViewDir::Right;
    case Part::NegX: return ViewDir::Left;
    case Part::PosY: return ViewDir::Top;
    case Part::NegY: return ViewDir::Bottom;
    case Part::PosZ: return ViewDir::Back;
    case Part::NegZ: return ViewDir::Front;
    default: return ViewDir::Front;
    }
}
