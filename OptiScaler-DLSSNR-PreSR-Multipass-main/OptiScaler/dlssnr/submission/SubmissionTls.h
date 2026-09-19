#pragma once
namespace DlssNr::Submission
{
// When non-zero, CreateCommandList hooks must not wrap (continuation alloc inside Split).
inline thread_local int g_suppressProxyWrap = 0;

struct SuppressProxyWrap
{
    SuppressProxyWrap() { ++g_suppressProxyWrap; }
    ~SuppressProxyWrap() { --g_suppressProxyWrap; }
    SuppressProxyWrap(const SuppressProxyWrap &) = delete;
    SuppressProxyWrap &operator=(const SuppressProxyWrap &) = delete;
};
} // namespace DlssNr::Submission
