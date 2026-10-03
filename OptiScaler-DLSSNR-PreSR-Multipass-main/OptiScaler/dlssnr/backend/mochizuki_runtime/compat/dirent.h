#pragma once
// MSVC has no dirent.h. telemetry.hpp only reads Linux sysfs, so on Windows no directory ever opens.
struct dirent
{
    char d_name[1];
};
struct DIR;
inline DIR* opendir(const char*) { return nullptr; }
inline dirent* readdir(DIR*) { return nullptr; }
inline int closedir(DIR*) { return 0; }
