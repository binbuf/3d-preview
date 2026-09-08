#pragma once

// Thin framing helpers used identically by both the host and the worker --
// this is why they live in shared/model-core (consumed by both processes),
// not shared/import-broker (host-side only). Implementation is a .cpp, not
// header-only, to avoid pulling <windows.h> into every header consumer.

#include "model_core/ControlProtocol.h"

#include <windows.h>

#include <cstddef>
#include <optional>
#include <vector>

namespace model_core {

bool WriteControlMessage(HANDLE pipe, ControlOpcode opcode, const void* payload,
                          uint32_t payloadSize);

struct ReceivedControlMessage {
    ControlMessageHeader header;
    std::vector<std::byte> payload;
};

// Reads exactly one framed message. Rejects (returns std::nullopt) before
// reading the payload if the declared payloadSize exceeds
// kMaxControlPayloadBytes, so a corrupt/oversized declared size can never
// drive an unbounded allocation or read.
std::optional<ReceivedControlMessage> ReadControlMessage(HANDLE pipe);

} // namespace model_core
