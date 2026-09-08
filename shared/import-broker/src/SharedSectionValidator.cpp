#include "import_broker/SharedSectionValidator.h"

#include "model_core/Checksum.h"
#include "model_core/VertexLayouts.h"
#include "platform/CheckedMath.h"

#include <cstring>

namespace import_broker {

namespace {

using model_core::ChunkDescriptor;
using model_core::ChunkTopology;
using model_core::Fnv1a64;
using model_core::ImportErrorCode;
using model_core::SectionHeader;
using model_core::VertexStrideForLayout;
using platform::CheckedAdd;
using platform::CheckedMultiply;

ValidationResult Reject(ImportErrorCode code, std::string message)
{
    ValidationResult result;
    result.ok = false;
    result.errorCode = code;
    result.diagnosticMessage = std::move(message);
    return result;
}

} // namespace

ValidationResult ValidateAndCopySection(std::span<const std::byte> sectionView,
                                         uint64_t expectedGenerationId, uint32_t maxChunkCount)
{
    // 1. The section must be at least large enough to hold a header before
    // any field of it is read.
    if (sectionView.size() < sizeof(SectionHeader)) {
        return Reject(ImportErrorCode::MalformedData, "section smaller than header");
    }

    // 2. Copy the header into a local, private copy -- validate the copy,
    // never the shared view directly, even on the honest path.
    SectionHeader header{};
    std::memcpy(&header, sectionView.data(), sizeof(header));

    // 3.
    if (header.magic != model_core::kSectionMagic) {
        return Reject(ImportErrorCode::MalformedData, "bad magic");
    }

    // 4. Never attempt to interpret an unrecognized protocol version.
    if (header.protocolVersion != model_core::kCurrentProtocolVersion) {
        return Reject(ImportErrorCode::ImportProtocolViolation, "unrecognized protocol version");
    }

    // 5. Bounds-check the header's self-declared length against the actual
    // mapped size before trusting it for anything else.
    if (header.sectionLength > sectionView.size()) {
        return Reject(ImportErrorCode::MalformedData, "sectionLength exceeds mapped view");
    }

    // 6.
    if (header.sectionLength < sizeof(SectionHeader)) {
        return Reject(ImportErrorCode::MalformedData, "sectionLength smaller than header");
    }

    // 7. Sane cap enforced before any allocation sized by chunkCount.
    if (header.chunkCount > maxChunkCount) {
        return Reject(ImportErrorCode::ResourceLimit, "chunkCount exceeds cap");
    }

    // 8. Descriptor table must fit within the already-bounds-checked
    // section length, via checked arithmetic.
    auto descriptorTableBytes
        = CheckedMultiply(static_cast<uint64_t>(header.chunkCount), model_core::kChunkDescriptorSize);
    if (!descriptorTableBytes) {
        return Reject(ImportErrorCode::MalformedData, "chunkCount * descriptor size overflows");
    }
    auto needed = CheckedAdd(model_core::kSectionHeaderSize, *descriptorTableBytes);
    if (!needed) {
        return Reject(ImportErrorCode::MalformedData, "header + descriptor table size overflows");
    }

    // 9.
    if (*needed > header.sectionLength) {
        return Reject(ImportErrorCode::MalformedData, "descriptor table exceeds sectionLength");
    }

    // 10. Recompute the section checksum over [header, sectionLength).
    auto payloadRegion = sectionView.subspan(sizeof(SectionHeader),
                                              header.sectionLength - sizeof(SectionHeader));
    uint64_t recomputedChecksum = Fnv1a64(payloadRegion);
    if (recomputedChecksum != header.sectionChecksum) {
        return Reject(ImportErrorCode::MalformedData, "section checksum mismatch");
    }

    // 11. Staleness guard.
    if (header.generationId != expectedGenerationId) {
        return Reject(ImportErrorCode::ImportProtocolViolation, "generation ID mismatch");
    }

    ValidationResult result;
    result.chunks.reserve(header.chunkCount);

    for (uint32_t i = 0; i < header.chunkCount; ++i) {
        size_t descriptorOffset = sizeof(SectionHeader) + i * model_core::kChunkDescriptorSize;

        // 12a. Copy this descriptor into a local, private copy.
        ChunkDescriptor descriptor{};
        std::memcpy(&descriptor, sectionView.data() + descriptorOffset, sizeof(descriptor));

        // 12b.
        auto payloadEnd = CheckedAdd(descriptor.normalizedRangeOffset, descriptor.byteSize);
        if (!payloadEnd || *payloadEnd > header.sectionLength) {
            return Reject(ImportErrorCode::MalformedData, "chunk payload exceeds sectionLength");
        }

        // 12c.
        if (descriptor.byteSize != descriptor.normalizedRangeLength) {
            return Reject(ImportErrorCode::MalformedData, "byteSize/normalizedRangeLength mismatch");
        }

        // 12d.
        if (descriptor.topology != ChunkTopology::TriangleList
            && descriptor.topology != ChunkTopology::PointList) {
            return Reject(ImportErrorCode::MalformedData, "unrecognized topology");
        }

        // 12e. Unrecognized layout ID is never guessed.
        size_t stride = VertexStrideForLayout(static_cast<model_core::VertexLayoutId>(
            descriptor.vertexLayoutId));
        if (stride == 0) {
            return Reject(ImportErrorCode::MalformedData, "unrecognized vertex layout ID");
        }

        // 12f. Expected byte size from vertex/index counts, via checked
        // arithmetic. PointList chunks must declare indexCount == 0 -- no
        // index buffer for points, per this chunk's scope.
        auto vertexBytes = CheckedMultiply(static_cast<uint64_t>(descriptor.vertexCount), stride);
        if (!vertexBytes) {
            return Reject(ImportErrorCode::MalformedData, "vertexCount * stride overflows");
        }

        uint64_t expectedByteSize = 0;
        if (descriptor.topology == ChunkTopology::TriangleList) {
            auto indexBytes = CheckedMultiply(static_cast<uint64_t>(descriptor.indexCount),
                                               static_cast<uint64_t>(sizeof(uint32_t)));
            if (!indexBytes) {
                return Reject(ImportErrorCode::MalformedData, "indexCount * 4 overflows");
            }
            auto total = CheckedAdd(*vertexBytes, *indexBytes);
            if (!total) {
                return Reject(ImportErrorCode::MalformedData, "vertex + index bytes overflows");
            }
            expectedByteSize = *total;
        } else {
            if (descriptor.indexCount != 0) {
                return Reject(ImportErrorCode::MalformedData, "PointList chunk declares indices");
            }
            expectedByteSize = *vertexBytes;
        }

        if (expectedByteSize != descriptor.byteSize) {
            return Reject(ImportErrorCode::MalformedData, "byteSize does not match declared counts");
        }

        // 12g.
        if (descriptor.dependencyCount > model_core::kMaxDependencyIds) {
            return Reject(ImportErrorCode::MalformedData, "dependencyCount exceeds cap");
        }

        // 12h. Recompute the per-chunk checksum over the validated payload
        // range.
        auto chunkPayloadView
            = sectionView.subspan(descriptor.normalizedRangeOffset, descriptor.byteSize);
        uint64_t recomputedChunkChecksum = Fnv1a64(chunkPayloadView);
        if (recomputedChunkChecksum != descriptor.chunkChecksum) {
            return Reject(ImportErrorCode::MalformedData, "chunk checksum mismatch");
        }

        // 12i. Only now, copy the payload into host-owned private memory.
        std::vector<std::byte> payload(chunkPayloadView.begin(), chunkPayloadView.end());
        result.chunks.push_back(ValidatedChunk{ descriptor, std::move(payload) });
    }

    // 13. Every chunk passed.
    result.ok = true;
    result.errorCode = ImportErrorCode::None;
    return result;
}

} // namespace import_broker
