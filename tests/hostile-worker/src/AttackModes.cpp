#include "AttackModes.h"

#include "SyntheticSceneGenerator.h"

#include "model_core/Checksum.h"
#include "model_core/ControlChannelIo.h"
#include "model_core/ControlProtocol.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"
#include "platform/MappedView.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <variant>

namespace hostile_worker {

namespace {

// Delay before RunMutateAfterReady corrupts the section, comfortably longer
// than any plausible host validation pass over the ~1KB honest fixture
// (memcpy + FNV-1a64 + a handful of small vector allocations -- low
// single-digit milliseconds even pessimistically). 2+ orders of magnitude
// of headroom, consistent with this codebase's existing timeout precedents
// (SandboxLaunchTests.cpp's 500ms/5000ms waits).
constexpr DWORD kCorruptionDelayMs = 300;

struct RequestAndSection {
    model_core::StartGenerationRequest request;
    platform::MappedView view;
};

// Common preamble shared by all four attack modes: read one
// StartGenerationRequest from the inherited stdin pipe and map the
// inherited shared section. Deliberately not calling into
// GenerationWorker.cpp -- see AttackModes.h.
std::optional<RequestAndSection> ReadRequestAndMapSection()
{
    HANDLE stdIn = GetStdHandle(STD_INPUT_HANDLE);
    if (stdIn == nullptr || stdIn == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    auto received = model_core::ReadControlMessage(stdIn);
    if (!received
        || received->header.opcode != static_cast<uint32_t>(model_core::ControlOpcode::StartGeneration)
        || received->payload.size() != sizeof(model_core::StartGenerationRequest)) {
        return std::nullopt;
    }

    model_core::StartGenerationRequest request{};
    std::memcpy(&request, received->payload.data(), sizeof(request));

    HANDLE section = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.sectionHandleValue));
    auto view = platform::MappedView::Map(section, FILE_MAP_WRITE | FILE_MAP_READ,
                                           static_cast<SIZE_T>(request.sectionByteCapacity));
    if (!view) {
        return std::nullopt;
    }

    RequestAndSection result;
    result.request = request;
    result.view = std::move(view);
    return result;
}

bool SendChunksReady(uint64_t generationId, uint32_t chunkCount, uint64_t sectionBytesWritten)
{
    HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (stdOut == nullptr || stdOut == INVALID_HANDLE_VALUE) {
        return false;
    }

    model_core::ChunksReadyNotice notice{};
    notice.generationId = generationId;
    notice.chunkCount = chunkCount;
    notice.sectionBytesWritten = sectionBytesWritten;
    return model_core::WriteControlMessage(stdOut, model_core::ControlOpcode::ChunksReady, &notice,
                                            sizeof(notice));
}

// Hand-builds a single-PointList-chunk, single-vertex section directly into
// `destination`, honest in every respect except whatever the caller
// overrides via `corrupt` after the honest fields are set but before
// checksums are computed over what was actually written -- matching
// SyntheticSceneGenerator's "header written last" ordering.
template <typename CorruptFn>
uint64_t BuildLyingSingleChunkSection(std::span<std::byte> destination, uint64_t generationId,
                                       CorruptFn corrupt)
{
    using namespace model_core;

    VertexPositionOnlyF32 vertex{ 7.0f, 8.0f, 9.0f };

    uint64_t payloadOffset = kSectionHeaderSize + kChunkDescriptorSize;
    uint64_t sectionLength = payloadOffset + sizeof(vertex);

    std::memcpy(destination.data() + payloadOffset, &vertex, sizeof(vertex));

    ChunkDescriptor descriptor{};
    descriptor.normalizedRangeOffset = payloadOffset;
    descriptor.normalizedRangeLength = sizeof(vertex);
    descriptor.topology = ChunkTopology::PointList;
    descriptor.indexCount = 0;
    descriptor.vertexCount = 1;
    descriptor.vertexLayoutId = static_cast<uint32_t>(VertexLayoutId::PositionOnly_F32);
    descriptor.lodLevel = 0;
    descriptor.chunkId = 1;
    descriptor.byteSize = sizeof(vertex);
    descriptor.dependencyCount = 0;
    descriptor.chunkChecksum = Fnv1a64(destination.subspan(payloadOffset, sizeof(vertex)));

    // The caller's lie: e.g. an oversized byteSize, or a bogus
    // vertexLayoutId. Applied after honest defaults, before the section
    // checksum below is computed over whatever ends up actually written.
    corrupt(descriptor, sectionLength);

    std::memcpy(destination.data() + kSectionHeaderSize, &descriptor, sizeof(descriptor));

    SectionHeader header{};
    header.magic = kSectionMagic;
    header.protocolVersion = kCurrentProtocolVersion;
    header.generationId = generationId;
    header.sectionLength = sectionLength;
    header.chunkCount = 1;
    header.reserved = 0;
    header.sectionChecksum
        = Fnv1a64(destination.subspan(kSectionHeaderSize, sectionLength - kSectionHeaderSize));

    std::memcpy(destination.data(), &header, sizeof(header));

    return sectionLength;
}

} // namespace

int RunMutateAfterReady()
{
    auto requestAndSection = ReadRequestAndMapSection();
    if (!requestAndSection) {
        return 1;
    }
    auto& [request, view] = *requestAndSection;

    auto result = import_worker::GenerateSyntheticScene(view.bytes(), request.generationId,
                                                          request.sceneVariant, request.maxChunkCount);
    const auto* info = std::get_if<import_worker::GeneratedSectionInfo>(&result);
    if (info == nullptr) {
        return 1;
    }

    if (!SendChunksReady(request.generationId, info->chunkCount, info->sectionBytesWritten)) {
        return 1;
    }

    // Deliberately single-threaded: the corruption below happens strictly
    // before this function (and therefore main()) returns, on the same
    // thread -- so the process's exit is a genuine Win32 happens-before
    // guarantee that the corruption has already occurred. A detached
    // background thread would not be ordered against process exit and
    // would reintroduce exactly the kind of race this test needs to avoid.
    Sleep(kCorruptionDelayMs);

    // Corrupt vertex 0's px, the first 4 bytes of chunk 0's payload, at the
    // fixed offset SyntheticSceneGenerator.cpp itself computes as
    // chunk0Offset for a 2-chunk scene.
    uint64_t chunk0Offset = model_core::kSectionHeaderSize + 2 * model_core::kChunkDescriptorSize;
    float corruptedPx = -999.0f;
    std::memcpy(view.bytes().data() + chunk0Offset, &corruptedPx, sizeof(corruptedPx));

    return 0;
}

int RunReplayGeneration()
{
    auto requestAndSection = ReadRequestAndMapSection();
    if (!requestAndSection) {
        return 1;
    }
    auto& [request, view] = *requestAndSection;

    // Lie to the generator itself about which generation this is for --
    // every checksum ends up internally consistent (checksums don't cover
    // the generationId field), but the stamped ID is wrong relative to what
    // the host asked for.
    uint64_t staleGenerationId = request.generationId + 999;
    auto result = import_worker::GenerateSyntheticScene(view.bytes(), staleGenerationId,
                                                          request.sceneVariant, request.maxChunkCount);
    const auto* info = std::get_if<import_worker::GeneratedSectionInfo>(&result);
    if (info == nullptr) {
        return 1;
    }

    // The outer notice correctly claims the requested generation is done --
    // only the section's own stamped ID lies.
    if (!SendChunksReady(request.generationId, info->chunkCount, info->sectionBytesWritten)) {
        return 1;
    }
    return 0;
}

int RunLieOffset()
{
    auto requestAndSection = ReadRequestAndMapSection();
    if (!requestAndSection) {
        return 1;
    }
    auto& [request, view] = *requestAndSection;

    uint64_t sectionLength = BuildLyingSingleChunkSection(
        view.bytes(), request.generationId,
        [](model_core::ChunkDescriptor& descriptor, uint64_t honestSectionLength) {
            // Claim a payload range that overflows well past sectionLength.
            descriptor.byteSize = honestSectionLength + 1000;
            descriptor.normalizedRangeLength = descriptor.byteSize;
        });

    if (!SendChunksReady(request.generationId, 1, sectionLength)) {
        return 1;
    }
    return 0;
}

int RunLieLayout()
{
    auto requestAndSection = ReadRequestAndMapSection();
    if (!requestAndSection) {
        return 1;
    }
    auto& [request, view] = *requestAndSection;

    uint64_t sectionLength = BuildLyingSingleChunkSection(
        view.bytes(), request.generationId,
        [](model_core::ChunkDescriptor& descriptor, uint64_t /*honestSectionLength*/) {
            // Outside the closed VertexLayoutId enumeration.
            descriptor.vertexLayoutId = 9999;
        });

    if (!SendChunksReady(request.generationId, 1, sectionLength)) {
        return 1;
    }
    return 0;
}

} // namespace hostile_worker
