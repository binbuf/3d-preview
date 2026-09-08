#include "AttackModes.h"

#include <cstring>

namespace {

bool ArgEquals(const char* arg, const char* value)
{
    return std::strcmp(arg, value) == 0;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc < 2) {
        return 1;
    }

    if (ArgEquals(argv[1], "--mutate-after-ready")) {
        return hostile_worker::RunMutateAfterReady();
    }

    if (ArgEquals(argv[1], "--replay-generation")) {
        return hostile_worker::RunReplayGeneration();
    }

    if (ArgEquals(argv[1], "--lie-offset")) {
        return hostile_worker::RunLieOffset();
    }

    if (ArgEquals(argv[1], "--lie-layout")) {
        return hostile_worker::RunLieLayout();
    }

    return 1;
}
