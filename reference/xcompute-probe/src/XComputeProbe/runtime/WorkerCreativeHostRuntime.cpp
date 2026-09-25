#include "pch.h"
#include "WorkerCreativeHostRuntime.h"

#include "WorkerCreativeHostProfile.generated.h"

namespace XComputeProbe
{
    std::wstring WorkerCreativeHostProfileJson()
    {
        return WorkerCreativeHostProfileJsonValue;
    }

    std::wstring WorkerCreativeHostProfileSourceSha256()
    {
        return WorkerCreativeHostProfileSourceSha256Value;
    }

    std::wstring WorkerCreativeHostProfileCanonicalSha256()
    {
        return WorkerCreativeHostProfileCanonicalSha256Value;
    }
}
