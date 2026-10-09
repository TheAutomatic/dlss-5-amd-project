#pragma once
#include <algorithm>

namespace DlssNr::Person
{
// A small CPU budget for a background feature, independent of CPU model/SMT.
// Leave most hardware threads to the game and do not impose affinity on hybrid CPUs.
inline unsigned WorkerThreads(unsigned hardwareThreads)
{
    return std::clamp(hardwareThreads / 4, 2u, 4u);
}
}
