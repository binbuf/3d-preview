#include "GenerationWorker.h"

#include "SyntheticSceneGenerator.h"

#include "model_core/ControlChannelIo.h"
#include "model_core/ControlProtocol.h"
#include "platform/MappedView.h"

#include <windows.h>

#include <cstring>
#include <variant>

namespace import_worker {

int RunGeneration()
{
    HANDLE stdIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (stdIn == nullptr || stdIn == INVALID_HANDLE_VALUE || stdOut == nullptr
        || stdOut == INVALID_HANDLE_VALUE) {
        return 1;
    }

    auto received = model_core::ReadControlMessage(stdIn);
    if (!received
        || received->header.opcode != static_cast<uint32_t>(model_core::ControlOpcode::StartGeneration)
        || received->payload.size() != sizeof(model_core::StartGenerationRequest)) {
        // Honest-worker protocol sanity check, not adversarial handling.
        return 1;
    }

    model_core::StartGenerationRequest request{};
    std::memcpy(&request, received->payload.data(), sizeof(request));

    HANDLE section = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.sectionHandleValue));
    auto view = platform::MappedView::Map(section, FILE_MAP_WRITE | FILE_MAP_READ,
                                           static_cast<SIZE_T>(request.sectionByteCapacity));
    if (!view) {
        model_core::GenerationErrorNotice notice{};
        notice.generationId = request.generationId;
        notice.errorCode = static_cast<uint32_t>(model_core::ImportErrorCode::InternalImporterFailure);
        model_core::WriteControlMessage(stdOut, model_core::ControlOpcode::GenerationError, &notice,
                                         sizeof(notice));
        return 1;
    }

    auto result = GenerateSyntheticScene(view.bytes(), request.generationId, request.sceneVariant,
                                          request.maxChunkCount);

    if (const auto* errorCode = std::get_if<model_core::ImportErrorCode>(&result)) {
        model_core::GenerationErrorNotice notice{};
        notice.generationId = request.generationId;
        notice.errorCode = static_cast<uint32_t>(*errorCode);
        model_core::WriteControlMessage(stdOut, model_core::ControlOpcode::GenerationError, &notice,
                                         sizeof(notice));
        return 1;
    }

    const auto& info = std::get<GeneratedSectionInfo>(result);
    model_core::ChunksReadyNotice notice{};
    notice.generationId = request.generationId;
    notice.chunkCount = info.chunkCount;
    notice.sectionBytesWritten = info.sectionBytesWritten;
    model_core::WriteControlMessage(stdOut, model_core::ControlOpcode::ChunksReady, &notice,
                                     sizeof(notice));
    return 0;
}

} // namespace import_worker
