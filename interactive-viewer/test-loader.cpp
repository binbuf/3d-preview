#include "framework.h"
#include "Model.h"

#include <cstdio>

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        wprintf(L"usage: test-loader <file.glb> [file2.glb ...]\n");
        return 1;
    }
    for (int index = 1; index < argc; ++index)
    {
        const auto cancel = std::make_shared<std::atomic_bool>(false);
        const LoadResult result = LoadGlb(argv[index], cancel, [](const wchar_t*) {});
        if (result.succeeded)
        {
            wprintf(L"SUCCESS %s: %llu triangles, %zu vertices, %zu indices, bounds (%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f)\n",
                argv[index], result.model->triangleCount, result.model->vertices.size(), result.model->indices.size(),
                result.model->boundsMin.x, result.model->boundsMin.y, result.model->boundsMin.z,
                result.model->boundsMax.x, result.model->boundsMax.y, result.model->boundsMax.z);
            if (!result.model->warning.empty()) wprintf(L"  warning: %s\n", result.model->warning.c_str());
        }
        else if (result.cancelled)
        {
            wprintf(L"CANCELLED %s\n", argv[index]);
        }
        else
        {
            wprintf(L"FAILURE %s: %s\n  details: %s\n", argv[index], result.summary.c_str(), result.details.c_str());
        }
    }
    return 0;
}
