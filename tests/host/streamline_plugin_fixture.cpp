#include <Windows.h>
#include <cstring>
#ifndef FIXTURE_ID
#define FIXTURE_ID 11
#endif
static __declspec(noinline) int Value() { return FIXTURE_ID; }
extern "C" __declspec(dllexport) __declspec(noinline) void* slGetPluginFunction(const char* name)
{
    if (name && std::strcmp(name, "value") == 0) return reinterpret_cast<void*>(&Value);
    if (name && std::strcmp(name, "missing") == 0) return nullptr;
    return nullptr;
}
