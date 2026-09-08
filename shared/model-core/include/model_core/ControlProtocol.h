#pragma once

#include "model_core/ImportError.h"

#include <cstdint>

namespace model_core {

enum class ControlOpcode : uint32_t {
    StartGeneration = 1, // host -> worker
    ChunksReady = 2,     // worker -> host
    GenerationError = 3, // worker -> host
};

// Bounded so a corrupt/oversized declared payload size can never drive an
// unbounded allocation or read in ControlChannelIo::ReadControlMessage.
constexpr uint32_t kMaxControlPayloadBytes = 256;

#pragma pack(push, 1)

struct ControlMessageHeader {
    uint32_t opcode;      // ControlOpcode value
    uint32_t payloadSize; // exact byte size of the payload that follows; must be <= kMaxControlPayloadBytes
};
static_assert(sizeof(ControlMessageHeader) == 8, "ControlMessageHeader layout changed");

enum : uint32_t {
    kSceneVariant_CubeAndPointCluster = 1,
};

struct StartGenerationRequest {
    uint64_t generationId;
    uint32_t sceneVariant;       // selects which synthetic scene the worker fabricates
    uint32_t reserved0;
    uint64_t sectionHandleValue; // the inherited shared-section HANDLE's numeric value, as an
                                   // opaque fixed-width integer (never a raw HANDLE/void* in the
                                   // struct, for explicit/portable layout) -- valid because
                                   // Windows handle inheritance preserves the numeric value
    uint64_t sectionByteCapacity; // how many bytes of the section the worker may use
    uint32_t maxChunkCount;       // sanity cap on chunk count the worker may emit
    uint32_t reserved1;
};
static_assert(sizeof(StartGenerationRequest) == 40, "StartGenerationRequest layout changed");

struct ChunksReadyNotice {
    uint64_t generationId;
    uint32_t chunkCount;
    uint32_t reserved0;
    uint64_t sectionBytesWritten; // diagnostic only -- the validator always re-derives and
                                    // bounds-checks the authoritative length from
                                    // SectionHeader::sectionLength itself, never trusts this
                                    // separate claim
};
static_assert(sizeof(ChunksReadyNotice) == 24, "ChunksReadyNotice layout changed");

struct GenerationErrorNotice {
    uint64_t generationId;
    uint32_t errorCode; // ImportErrorCode value
    uint32_t reserved0;
};
static_assert(sizeof(GenerationErrorNotice) == 16, "GenerationErrorNotice layout changed");

#pragma pack(pop)

} // namespace model_core
