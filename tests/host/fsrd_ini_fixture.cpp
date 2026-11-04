// Loaded only as a real module identity for the production Util::DllPath().
// No Config, INI, profile, or platform behavior is replaced by this fixture.
extern "C" __declspec(dllexport) int FsrdIniFixtureIdentity()
{
    return 1;
}
