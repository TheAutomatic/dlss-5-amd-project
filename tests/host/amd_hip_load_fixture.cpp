// Only a harmless loader fixture. Does not implement HIP or interact with a GPU.
#ifdef AMD_HIP_DEPENDENCY_FIXTURE
extern "C" __declspec(dllexport) int hipFixtureDependency() { return 42; }
#else
extern "C" __declspec(dllimport) int hipFixtureDependency();
extern "C" __declspec(dllexport) int hipFixtureValue() { return hipFixtureDependency(); }
#endif
