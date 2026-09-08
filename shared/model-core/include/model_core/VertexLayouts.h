#pragma once

#include <cstddef>
#include <cstdint>

namespace model_core {

// Closed enumeration -- the wire format carries only this numeric ID, never
// a raw stride/format string the receiver would have to interpret.
enum class VertexLayoutId : uint32_t {
    Unknown = 0,
    PositionOnly_F32 = 1,      // 12 bytes/vertex
    PositionNormalUv0_F32 = 2, // 32 bytes/vertex
};

#pragma pack(push, 1)

struct VertexPositionOnlyF32 {
    float x, y, z;
};
static_assert(sizeof(VertexPositionOnlyF32) == 12, "VertexPositionOnlyF32 layout changed");

struct VertexPositionNormalUv0F32 {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};
static_assert(sizeof(VertexPositionNormalUv0F32) == 32, "VertexPositionNormalUv0F32 layout changed");

#pragma pack(pop)

// Host-trusted lookup: returns 0 for VertexLayoutId::Unknown and for any
// value outside the closed enumeration (including forward-incompatible
// future IDs a compromised or newer-than-us worker might send). The
// validator must never guess a stride for an unrecognized ID.
constexpr size_t VertexStrideForLayout(VertexLayoutId id) noexcept
{
    switch (id) {
    case VertexLayoutId::PositionOnly_F32:
        return sizeof(VertexPositionOnlyF32);
    case VertexLayoutId::PositionNormalUv0_F32:
        return sizeof(VertexPositionNormalUv0F32);
    default:
        return 0;
    }
}

} // namespace model_core
