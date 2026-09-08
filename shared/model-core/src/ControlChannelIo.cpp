#include "model_core/ControlChannelIo.h"

namespace model_core {

namespace {

bool WriteAll(HANDLE pipe, const void* data, DWORD size)
{
    const auto* bytes = static_cast<const BYTE*>(data);
    DWORD totalWritten = 0;
    while (totalWritten < size) {
        DWORD written = 0;
        if (!WriteFile(pipe, bytes + totalWritten, size - totalWritten, &written, nullptr)) {
            return false;
        }
        if (written == 0) {
            return false;
        }
        totalWritten += written;
    }
    return true;
}

bool ReadAll(HANDLE pipe, void* data, DWORD size)
{
    auto* bytes = static_cast<BYTE*>(data);
    DWORD totalRead = 0;
    while (totalRead < size) {
        DWORD bytesRead = 0;
        if (!ReadFile(pipe, bytes + totalRead, size - totalRead, &bytesRead, nullptr)) {
            return false;
        }
        if (bytesRead == 0) {
            return false;
        }
        totalRead += bytesRead;
    }
    return true;
}

} // namespace

bool WriteControlMessage(HANDLE pipe, ControlOpcode opcode, const void* payload,
                          uint32_t payloadSize)
{
    if (payloadSize > kMaxControlPayloadBytes) {
        return false;
    }

    ControlMessageHeader header{};
    header.opcode = static_cast<uint32_t>(opcode);
    header.payloadSize = payloadSize;

    if (!WriteAll(pipe, &header, sizeof(header))) {
        return false;
    }
    if (payloadSize > 0 && !WriteAll(pipe, payload, payloadSize)) {
        return false;
    }
    return true;
}

std::optional<ReceivedControlMessage> ReadControlMessage(HANDLE pipe)
{
    ControlMessageHeader header{};
    if (!ReadAll(pipe, &header, sizeof(header))) {
        return std::nullopt;
    }

    if (header.payloadSize > kMaxControlPayloadBytes) {
        return std::nullopt;
    }

    ReceivedControlMessage message;
    message.header = header;
    message.payload.resize(header.payloadSize);
    if (header.payloadSize > 0 && !ReadAll(pipe, message.payload.data(), header.payloadSize)) {
        return std::nullopt;
    }

    return message;
}

} // namespace model_core
