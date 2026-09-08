// Proves the Catch2/vcpkg/MSBuild test harness wiring itself, not product
// code. Gate 2 workstream B replaces/extends this once shared/platform's
// RAII/checked-math/generation primitives exist to test for real.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("unit test harness runs", "[harness]")
{
    REQUIRE(1 + 1 == 2);
}

int main(int argc, char* argv[])
{
    return Catch::Session().run(argc, argv);
}
