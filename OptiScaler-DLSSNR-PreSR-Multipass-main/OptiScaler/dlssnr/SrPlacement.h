#pragma once
namespace DlssNr
{
// Freeze the order across an SR evaluate pair. A menu change between the two
// hooks takes effect on the next pair, never running NR twice (or zero times).
struct SrPlacement
{
    const void* pending = nullptr;
    bool selectedBefore = true;
    bool Select(const void* parameters, bool beforeCall, bool requestedBefore)
    {
        if (beforeCall)
        {
            pending = parameters;
            selectedBefore = requestedBefore;
            return selectedBefore;
        }
        const bool selected = pending == parameters ? selectedBefore : requestedBefore;
        pending = nullptr;
        return selected;
    }
};
} // namespace DlssNr
