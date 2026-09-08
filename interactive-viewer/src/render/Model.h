#pragma once

#include <DirectXMath.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ModelVertex
{
    DirectX::XMFLOAT3 position{};
    DirectX::XMFLOAT3 normal{};
    DirectX::XMFLOAT4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
};

// Scanned (not rendered) glTF/GLB metadata for the Information side panel. All of
// this comes from cheap lookups against the already-parsed glTF JSON — none of it
// requires decoding texture pixels or implementing animation/skinning playback,
// which stay deliberately out of scope for this preview slice (see README.md).
struct ModelStats
{
    // Mesh Data
    bool hasUv0 = false;
    bool hasUv1 = false;
    bool hasVertexColors = false;
    int materialCount = 0;

    // Texture Data: how many materials reference each PBR texture slot. glTF
    // packs metallic (B) and roughness (G) into one `metallicRoughnessTexture`,
    // so that single count stands in for both Specular/Metallic and
    // Gloss/Roughness. `hasConstant*` flags a factor-only (untextured) value
    // for the fields that have no natural "count" of their own.
    int albedoTextureCount = 0;
    int normalTextureCount = 0;
    int specularMetallicTextureCount = 0;
    int occlusionTextureCount = 0;
    int emissiveTextureCount = 0;
    bool hasConstantBaseColor = false;
    bool hasConstantEmissiveColor = false;
    bool hasConstantSpecularColor = false;
    bool hasTransparency = false;   // alphaMode != OPAQUE, or a base color factor alpha < 1

    // Animation Data
    int animationCount = 0;
    int skinCount = 0;
    int boneCount = 0;   // total joints across all skins (glTF has no separate "bone" concept)

    // Performance Data
    int drawCallCount = 0;

    // Scene Data
    int nodeCount = 0;
};

struct ModelData
{
    std::vector<ModelVertex> vertices;
    std::vector<std::uint32_t> indices;
    DirectX::XMFLOAT3 boundsMin{};
    DirectX::XMFLOAT3 boundsMax{};
    std::uint64_t triangleCount = 0;
    std::wstring warning;
    ModelStats stats;
};

struct LoadResult
{
    bool succeeded = false;
    bool cancelled = false;
    std::shared_ptr<ModelData> model;
    std::wstring summary;
    std::wstring details;
};

using LoadProgressCallback = std::function<void(const wchar_t*)>;

LoadResult LoadGlb(
    const std::wstring& path,
    const std::shared_ptr<std::atomic_bool>& cancel,
    const LoadProgressCallback& progress);

// Ray-versus-mesh intersection for click selection. The importer bakes node
// transforms, so vertices are already in world space and the ray is cast in
// world space. `direction` does not need to be normalized. Returns true and
// sets `hitDistance` to the entry distance along the normalized direction.
bool PickMesh(
    const ModelData& model,
    const DirectX::XMFLOAT3& origin,
    const DirectX::XMFLOAT3& direction,
    float& hitDistance);
