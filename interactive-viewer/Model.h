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

struct ModelData
{
    std::vector<ModelVertex> vertices;
    std::vector<std::uint32_t> indices;
    DirectX::XMFLOAT3 boundsMin{};
    DirectX::XMFLOAT3 boundsMax{};
    std::uint64_t triangleCount = 0;
    std::wstring warning;
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
