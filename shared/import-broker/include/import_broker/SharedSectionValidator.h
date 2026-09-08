#pragma once

// Host-side copy-then-validate acceptance path for a normalized-chunk
// shared section. Per .docs/design/02-system-architecture.md: "A shared
// section is not itself a trust boundary... the loader/broker pool
// therefore treats a chunk as untrusted right up until it has been copied."
//
// ValidateAndCopySection performs the full check sequence documented below
// before copying any payload bytes into host-owned memory, and never
// retains a reference into the caller's section view after returning -- the
// caller may safely UnmapViewOfFile immediately after this call returns.
//
// Fail-closed at the section level: any single check failure (header or any
// one chunk) rejects the whole batch, matching "the broker... does not
// retry the same failing stage in a loop." This validator is exercised only
// against honest (if malformed) input in this chunk; adversarial mutation/
// replay/spoofing proof is the deferred synthetic hostile-worker suite.

#include "model_core/ImportError.h"
#include "model_core/WireFormat.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace import_broker {

struct ValidatedChunk {
    model_core::ChunkDescriptor descriptor; // copied into private host memory
    std::vector<std::byte> payload;         // copied into private host memory
};

struct ValidationResult {
    bool ok = false;
    std::vector<ValidatedChunk> chunks;
    model_core::ImportErrorCode errorCode = model_core::ImportErrorCode::None;
    std::string diagnosticMessage; // developer-facing only, never parsed
};

// sectionView must be exactly the caller's actual MapViewOfFile size (never
// the section's own self-declared length).
ValidationResult ValidateAndCopySection(std::span<const std::byte> sectionView,
                                         uint64_t expectedGenerationId, uint32_t maxChunkCount);

} // namespace import_broker
