#pragma once

// Fabricates a small, bounded, deterministic synthetic scene standing in
// for a real parser -- no third-party format library is wired yet (that's
// Gate 3/4). Writes directly into the mapped shared-section view per the
// model_core wire format.

#include "model_core/ImportError.h"

#include <cstdint>
#include <span>
#include <variant>

namespace import_worker {

struct GeneratedSectionInfo {
    uint32_t chunkCount = 0;
    uint64_t sectionBytesWritten = 0;
};

// Returns an ImportErrorCode instead of a GeneratedSectionInfo if the fixed
// scene wouldn't fit in `destination`, `maxChunkCount` is too small for it,
// or `sceneVariant` is unrecognized.
std::variant<GeneratedSectionInfo, model_core::ImportErrorCode> GenerateSyntheticScene(
    std::span<std::byte> destination, uint64_t generationId, uint32_t sceneVariant,
    uint32_t maxChunkCount);

} // namespace import_worker
