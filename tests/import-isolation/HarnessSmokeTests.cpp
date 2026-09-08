// Proves the Catch2/vcpkg/MSBuild test harness wiring itself, not product
// code. This project is where Gate 2 workstream A's HostileWorkerTests.cpp
// lands once shared/import-broker's AppContainerLauncher and
// SharedSectionValidator exist (see .docs/design/03-file-formats-and-ingestion.md,
// "Verification": the hostile-worker suite belongs here, keyed to the wire-
// format protocol version, not duplicated per format).
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("import-isolation test harness runs", "[harness]")
{
    REQUIRE(1 + 1 == 2);
}

int main(int argc, char* argv[])
{
    return Catch::Session().run(argc, argv);
}
