// Pure data-layout tests for the Gate 2 workstream A wire format -- no
// process launch, no I/O. Proves the wire structs are exactly the
// documented byte layout, the checksum is deterministic/corruption-
// sensitive, closed-enum lookups reject unknown IDs, and checked arithmetic
// never silently wraps. See .docs/design/03-file-formats-and-ingestion.md
// ("Wire format") and shared/model-core/include/model_core/.

#include "model_core/Checksum.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"
#include "platform/CheckedMath.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <limits>

TEST_CASE("SectionHeader and ChunkDescriptor match the documented fixed-width wire layout",
          "[wire-format]")
{
    REQUIRE(sizeof(model_core::SectionHeader) == 40);
    REQUIRE(sizeof(model_core::ChunkDescriptor) == 92);

    CHECK(offsetof(model_core::SectionHeader, magic) == 0);
    CHECK(offsetof(model_core::SectionHeader, protocolVersion) == 4);
    CHECK(offsetof(model_core::SectionHeader, generationId) == 8);
    CHECK(offsetof(model_core::SectionHeader, sectionChecksum) == 32);

    CHECK(offsetof(model_core::ChunkDescriptor, topology) == 32);
    CHECK(offsetof(model_core::ChunkDescriptor, byteSize) == 56);
    CHECK(offsetof(model_core::ChunkDescriptor, chunkChecksum) == 84);
}

TEST_CASE("Fnv1a64 is deterministic and detects single-byte corruption", "[wire-format]")
{
    std::array<std::byte, 8> data{ std::byte{ 1 }, std::byte{ 2 }, std::byte{ 3 }, std::byte{ 4 },
                                    std::byte{ 5 }, std::byte{ 6 }, std::byte{ 7 }, std::byte{ 8 } };

    uint64_t first = model_core::Fnv1a64(data);
    uint64_t second = model_core::Fnv1a64(data);
    CHECK(first == second);

    data[3] = std::byte{ 0xFF };
    uint64_t corrupted = model_core::Fnv1a64(data);
    CHECK(corrupted != first);
}

TEST_CASE("VertexStrideForLayout returns 0 for Unknown and any value outside the closed enumeration",
          "[wire-format]")
{
    CHECK(model_core::VertexStrideForLayout(model_core::VertexLayoutId::Unknown) == 0);
    CHECK(model_core::VertexStrideForLayout(model_core::VertexLayoutId::PositionOnly_F32) == 12);
    CHECK(model_core::VertexStrideForLayout(model_core::VertexLayoutId::PositionNormalUv0_F32)
          == 32);

    auto outOfRange = static_cast<model_core::VertexLayoutId>(0xFFFFFFFFu);
    CHECK(model_core::VertexStrideForLayout(outOfRange) == 0);
}

TEST_CASE("CheckedAdd and CheckedMultiply detect overflow instead of wrapping", "[wire-format]")
{
    constexpr uint64_t kMax = (std::numeric_limits<uint64_t>::max)();

    CHECK_FALSE(platform::CheckedAdd(kMax, 1).has_value());
    CHECK_FALSE(platform::CheckedMultiply(kMax, 2).has_value());

    auto sum = platform::CheckedAdd(40ULL, 92ULL);
    REQUIRE(sum.has_value());
    CHECK(*sum == 132ULL);

    auto product = platform::CheckedMultiply(8ULL, 92ULL);
    REQUIRE(product.has_value());
    CHECK(*product == 736ULL);

    CHECK(platform::CheckedMultiply(0ULL, kMax).has_value());
}
